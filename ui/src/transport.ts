import { CapabilitySet } from './bridge/capabilities';
import { onEvent } from './bridge/events';
import { BridgeError, sonora } from './bridge/invoke';
import { type PlayerState } from './bridge/generated';
import { coverElement, element, formatTime } from './format';
import { type QueueModel } from './queue';

// The transport: the part of the interface that is about sound rather than
// about the bridge.
//
// It never asks the shell what the state is on a timer. The shell says so, four
// times a second, through player.state -- and the whole state each time rather
// than a delta, so a page that missed an event is out of date for 250 ms
// instead of wrong until the next reload.
//
// Two values are deliberately not driven by that event while the user is
// touching them: dragging the position bar or the volume would otherwise fight
// the incoming state, and the control would jump under the finger.
//
// Week 7 took the file-path box out of here. There is nowhere left to type a
// path, because there is no longer a bridge method that takes one: the library
// hands out ids and the queue is built from those.

function describe(error: unknown): string {
  if (error instanceof BridgeError) {
    return `${error.message} (code ${error.code})`;
  }
  return error instanceof Error ? error.message : String(error);
}

function button(label: string, title: string, onClick: () => void): HTMLButtonElement {
  const element = document.createElement('button');
  element.type = 'button';
  element.textContent = label;
  element.title = title;
  element.addEventListener('click', onClick);
  return element;
}

export class Transport {
  private readonly nowPlaying = element('div', 'transport-now');
  private readonly status = element('p', 'transport-status');
  private readonly elapsed = element('span', 'transport-elapsed');
  private readonly remaining = element('span', 'transport-remaining');
  private readonly position = element('input', 'transport-position');
  private readonly volume = element('input', 'transport-volume');
  private readonly playPause = element('button', 'transport-play');
  private readonly error = element('p', 'transport-error');

  private scrubbing = false;
  private adjustingVolume = false;
  private playing = false;
  private duration = 0;

  constructor(
    private readonly root: HTMLElement,
    private readonly queue: QueueModel,
  ) {}

  /** Draws nothing but an explanation when the shell has no transport. */
  unavailable(reason: string): void {
    this.root.replaceChildren(element('p', 'unavailable', reason));
  }

  async mount(capabilities: CapabilitySet): Promise<void> {
    if (!capabilities.has('player')) {
      this.unavailable(
        capabilities.isDisabled('player')
          ? 'Playback is switched off for this run.'
          : 'This shell has no playback.',
      );
      return;
    }

    this.build();
    onEvent('player.state', (payload) => this.render(payload.player));
    // The title and the cover come from the queue, not from the state event:
    // the state knows an index, the queue knows what is at it.
    this.queue.onChange(() => this.renderNowPlaying());

    // The first paint does not wait for an event: at four a second the first
    // one is up to 250 ms away, and a transport that appears blank and then
    // fills in looks broken rather than fast.
    try {
      const state = await sonora.player.getState();
      this.render(state.player);
    } catch (caught) {
      this.fail(describe(caught));
    }
  }

  private build(): void {
    this.position.type = 'range';
    this.position.min = '0';
    this.position.max = '1000';
    this.position.value = '0';
    this.position.addEventListener('pointerdown', () => {
      this.scrubbing = true;
    });
    const commitSeek = (): void => {
      if (!this.scrubbing) {
        return;
      }
      this.scrubbing = false;
      const fraction = Number(this.position.value) / 1000;
      void this.call(() =>
        sonora.player.seek({ positionMs: Math.round(fraction * this.duration) }),
      );
    };
    this.position.addEventListener('pointerup', commitSeek);
    this.position.addEventListener('change', commitSeek);

    this.volume.type = 'range';
    this.volume.min = '0';
    this.volume.max = '100';
    this.volume.value = '100';
    this.volume.addEventListener('pointerdown', () => {
      this.adjustingVolume = true;
    });
    this.volume.addEventListener('pointerup', () => {
      this.adjustingVolume = false;
    });
    this.volume.addEventListener('input', () => {
      void this.call(() => sonora.player.setVolume({ level: Number(this.volume.value) / 100 }));
    });

    this.playPause.type = 'button';
    this.playPause.textContent = 'Play';
    this.playPause.addEventListener('click', () => {
      void this.call(() => (this.playing ? sonora.player.pause() : sonora.player.play()));
    });

    const controls = element('div', 'transport-controls');
    controls.append(
      button('⏮', 'Previous', () => void this.call(() => sonora.player.previous())),
      this.playPause,
      button('⏭', 'Next', () => void this.call(() => sonora.player.next())),
      button('⏹', 'Stop', () => void this.call(() => sonora.player.stop())),
    );

    const scrubber = element('div', 'transport-scrubber');
    scrubber.append(this.elapsed, this.position, this.remaining);

    const volumeRow = element('label', 'transport-volume-row');
    volumeRow.append('Volume', this.volume);

    const middle = element('div', 'transport-middle');
    middle.append(controls, scrubber);

    this.root.replaceChildren(this.nowPlaying, middle, volumeRow, this.status, this.error);
    this.renderNowPlaying();
  }

  private renderNowPlaying(): void {
    const entry = this.queue.current;
    if (entry === undefined) {
      this.nowPlaying.replaceChildren(element('span', 'transport-idle', 'Nothing playing'));
      return;
    }
    const text = element('div', 'transport-labels');
    text.append(
      element('span', 'transport-title', entry.title),
      element('span', 'transport-artist', entry.artist),
    );
    this.nowPlaying.replaceChildren(
      coverElement(entry.artUrl, entry.title, 'cover cover-small'),
      text,
    );
  }

  private render(state: PlayerState): void {
    this.playing = state.state === 'playing';
    this.duration = state.durationMs;

    this.status.textContent =
      state.queueSize === 0
        ? 'Queue empty'
        : `${state.state} · ${state.trackIndex + 1} of ${state.queueSize}` +
          (state.trackChanges > 0 ? ` · ${state.trackChanges} gapless join(s)` : '') +
          (state.underruns > 0 ? ` · ${state.underruns} underrun(s)` : '');

    this.playPause.textContent = this.playing ? 'Pause' : 'Play';

    this.elapsed.textContent = formatTime(state.positionMs);
    this.remaining.textContent = formatTime(state.durationMs);

    // Not while the user is holding the control: an incoming state would drag
    // the thumb back to where playback actually is, under the finger.
    if (!this.scrubbing) {
      this.position.value =
        state.durationMs > 0
          ? String(Math.round((state.positionMs / state.durationMs) * 1000))
          : '0';
    }
    if (!this.adjustingVolume) {
      this.volume.value = String(Math.round(state.volume * 100));
    }

    this.error.textContent = state.lastError === '' ? '' : state.lastError;
  }

  private fail(message: string): void {
    this.error.textContent = message;
  }

  private async call(action: () => Promise<unknown>): Promise<void> {
    try {
      await action();
      this.error.textContent = '';
    } catch (caught) {
      this.fail(describe(caught));
    }
  }
}
