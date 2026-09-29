import { CapabilitySet } from './bridge/capabilities';
import { onEvent } from './bridge/events';
import { type PlayerState, type QueueEntry } from './bridge/generated';
import { sonora } from './bridge/invoke';
import { barMark, coverElement, element, formatDuration, formatTime, plural } from './format';

// The queue: what is going to play, and what is playing now.
//
// It is not an event. player.state says how big the queue is and which entry is
// current, and that is enough to know when to ask for its contents again --
// which is rarely, because a queue changes when somebody changes it. Pushing the
// whole queue four times a second would be a list of forty titles sent 240 times
// a minute so that it can look identical each time.

/**
 * The queue, shared by the panel that lists it and the transport that draws the
 * current track's cover and title.
 *
 * One fetch, two readers. Before this both asked for the queue on the same
 * event, and the two answers could disagree by one call.
 */
export class QueueModel {
  private entries: readonly QueueEntry[] = [];
  private listeners: Array<() => void> = [];
  private currentIndex = -1;
  private lastVersion = -1;
  private lastIndex = -2;
  private inFlight = false;
  private pending = false;
  private available = false;

  enable(capabilities: CapabilitySet): void {
    this.available = capabilities.has('player');
  }

  onChange(listener: () => void): void {
    this.listeners.push(listener);
  }

  get all(): readonly QueueEntry[] {
    return this.entries;
  }

  get index(): number {
    return this.currentIndex;
  }

  get current(): QueueEntry | undefined {
    return this.currentIndex >= 0 ? this.entries[this.currentIndex] : undefined;
  }

  /** Called on every player.state. Refetches only when the queue can have changed. */
  observe(state: PlayerState): void {
    // The version, not the size and not the index.
    //
    // This started as "refetch when queueSize changed", which is wrong in the
    // most ordinary case there is: play a track, then play another one. The
    // queue is replaced, so it goes from one entry to one entry and the playing
    // index stays 0 -- nothing to notice -- and the player bar cheerfully kept
    // the first track's title over the second track's audio. The native side
    // now counts the changes it makes, and this watches that.
    const changed = state.queueVersion !== this.lastVersion;
    const moved = state.trackIndex !== this.lastIndex;
    this.lastVersion = state.queueVersion;
    this.lastIndex = state.trackIndex;
    this.currentIndex = state.trackIndex;

    if (changed) {
      void this.refresh();
    } else if (moved) {
      // The contents did not change, only which one is playing.
      this.notify();
    }
  }

  async refresh(): Promise<void> {
    if (!this.available) {
      return;
    }
    if (this.inFlight) {
      // Remembered, not dropped. Two quick double-clicks used to leave the
      // queue showing the first one: the second refresh arrived while the first
      // was still in the air and was thrown away, and nothing ever asked again.
      this.pending = true;
      return;
    }

    this.inFlight = true;
    try {
      const result = await sonora.player.getQueue();
      this.entries = result.entries;
      this.notify();
    } catch {
      // A queue that could not be read is left as it was: the transport above it
      // is still working, and the next state event asks again.
    } finally {
      this.inFlight = false;
      if (this.pending) {
        this.pending = false;
        void this.refresh();
      }
    }
  }

  private notify(): void {
    for (const listener of this.listeners) {
      listener();
    }
  }
}

export class QueuePanel {
  private readonly now = element('div', 'now');
  private readonly headingLabel = element('span', undefined, 'Next up');
  private readonly left = element('span', 'queue-left');
  private readonly head = element('div', 'queue-head');
  private readonly list = element('div', 'queue-list');
  /** Minutes remaining, so the label is rewritten when it changes and not 4 times a second. */
  private lastLeft = -1;
  private elapsed = 0;

  constructor(
    private readonly root: HTMLElement,
    private readonly model: QueueModel,
  ) {}

  mount(capabilities: CapabilitySet): void {
    if (!capabilities.has('player')) {
      this.root.replaceChildren(element('p', 'unavailable', 'No playback in this shell.'));
      return;
    }

    this.head.append(this.headingLabel, this.left);
    this.root.replaceChildren(this.now, this.head, this.list);

    this.model.onChange(() => this.render());
    // Only for the "N min left" figure, which needs how far into the current
    // track the listener is. Recomputed on every state and written to the DOM
    // only when the minute changes -- the alternative is a text node rewritten
    // four times a second to say the same thing.
    onEvent('player.state', (payload) => {
      this.elapsed = payload.player.positionMs;
      this.renderRemaining();
    });
    this.render();
  }

  private render(): void {
    const entries = this.model.all;
    const current = this.model.current;

    this.renderNow(current);

    if (entries.length === 0) {
      this.head.hidden = true;
      this.list.replaceChildren(this.emptyState());
      return;
    }

    this.head.hidden = false;
    // "Next up" is a claim about what is below it. With played entries still in
    // the list it would be a false one, so in that case the heading says what it
    // actually is.
    this.headingLabel.textContent = this.model.index > 0 ? 'In the queue' : 'Next up';
    this.lastLeft = -1;
    this.renderRemaining();

    this.list.replaceChildren(
      ...entries.map((entry) => {
        const row = element('button', 'queue-row');
        row.type = 'button';
        if (entry === current) {
          row.dataset['current'] = 'true';
        }

        const labels = element('div', 'queue-labels');
        labels.append(
          element('span', 'queue-title', entry.title),
          element('span', 'queue-artist', entry.artist),
        );

        row.append(
          coverElement(entry.artUrl, entry.album || entry.artist || entry.title, 'cover cover-xs'),
          labels,
          element('span', 'queue-time', formatTime(entry.durationMs)),
        );
        // jumpTo rather than a pile of next() calls, and absolute rather than
        // relative, so it works while the player is idle too.
        row.addEventListener('click', () => {
          void sonora.player.jumpTo({ index: entry.index });
        });
        return row;
      }),
    );
  }

  private renderNow(current: QueueEntry | undefined): void {
    if (current === undefined) {
      this.now.hidden = true;
      return;
    }
    this.now.hidden = false;

    const labels = element('div', 'now-labels');
    labels.append(
      element('span', 'now-kicker', 'Now playing'),
      element('span', 'now-title', current.title),
      element(
        'span',
        'now-artist',
        [current.artist, current.album].filter((part) => part !== '').join(' · '),
      ),
    );
    this.now.replaceChildren(
      coverElement(current.artUrl, current.album || current.title, 'cover cover-lg'),
      labels,
    );
  }

  /** What is left of the queue from the current track on, current track included. */
  private renderRemaining(): void {
    const entries = this.model.all;
    if (entries.length === 0) {
      return;
    }
    const from = Math.max(this.model.index, 0);
    let remaining = 0;
    for (let index = from; index < entries.length; index += 1) {
      remaining += entries[index]?.durationMs ?? 0;
    }
    remaining = Math.max(0, remaining - this.elapsed);

    const minutes = Math.round(remaining / 60000);
    if (minutes === this.lastLeft) {
      return;
    }
    this.lastLeft = minutes;
    // A queue with nothing but untagged files has no durations to add up, and
    // "0 min left" over four tracks is worse than saying how many there are.
    this.left.textContent =
      remaining > 0
        ? `${formatDuration(remaining)} left`
        : plural(entries.length - from, 'track');
  }

  private emptyState(): HTMLElement {
    const empty = element('div', 'empty empty-small');
    empty.append(barMark('bars empty-mark', [0.3, 0.6, 0.42]));
    const text = element('div');
    text.append(
      element('p', 'empty-title', 'Nothing queued'),
      element(
        'p',
        'empty-body',
        'Double-click a track to play it, or use the plus button to add it to the end.',
      ),
    );
    empty.append(text);
    return empty;
  }
}
