import { CapabilitySet } from './bridge/capabilities';
import { type PlayerState, type QueueEntry } from './bridge/generated';
import { sonora } from './bridge/invoke';
import { coverElement, element, formatTime } from './format';

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
  private readonly heading = element('h2', 'queue-heading', 'Queue');
  private readonly list = element('div', 'queue-list');

  constructor(
    private readonly root: HTMLElement,
    private readonly model: QueueModel,
  ) {}

  mount(capabilities: CapabilitySet): void {
    if (!capabilities.has('player')) {
      this.root.replaceChildren(element('p', 'unavailable', 'No playback in this shell.'));
      return;
    }

    const clear = element('button', 'queue-clear', 'Clear');
    clear.type = 'button';
    clear.addEventListener('click', () => {
      void sonora.player.clearQueue();
    });

    const header = element('div', 'queue-header');
    header.append(this.heading, clear);
    this.root.replaceChildren(header, this.list);

    this.model.onChange(() => this.render());
    this.render();
  }

  private render(): void {
    const entries = this.model.all;
    const current = this.model.current;
    this.heading.textContent = entries.length === 0 ? 'Queue' : `Queue (${entries.length})`;

    if (entries.length === 0) {
      this.list.replaceChildren(
        element('p', 'queue-empty', 'Nothing queued. Double-click a track to play it.'),
      );
      return;
    }

    this.list.replaceChildren(
      ...entries.map((entry) => {
        const row = element('button', 'queue-row');
        row.type = 'button';
        if (entry === current) {
          row.dataset['current'] = 'true';
        }
        row.append(
          coverElement(entry.artUrl, entry.title, 'cover cover-tiny'),
          element('span', 'queue-title', entry.title),
          element('span', 'queue-artist', entry.artist),
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
}
