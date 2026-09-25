import { CapabilitySet } from './bridge/capabilities';
import { onEvent } from './bridge/events';
import { BridgeError, sonora } from './bridge/invoke';
import { type PlayerState } from './bridge/generated';

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

function formatTime(milliseconds: number): string {
  if (!Number.isFinite(milliseconds) || milliseconds < 0) {
    return '--:--';
  }
  const total = Math.floor(milliseconds / 1000);
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  return `${minutes}:${seconds.toString().padStart(2, '0')}`;
}

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
  private readonly root: HTMLElement;
  private readonly status = document.createElement('p');
  private readonly track = document.createElement('p');
  private readonly elapsed = document.createElement('span');
  private readonly remaining = document.createElement('span');
  private readonly position = document.createElement('input');
  private readonly volume = document.createElement('input');
  private readonly path = document.createElement('input');
  private readonly playPause = document.createElement('button');
  private readonly error = document.createElement('p');

  private scrubbing = false;
  private adjustingVolume = false;
  private playing = false;

  constructor(root: HTMLElement) {
    this.root = root;
  }

  /** Draws nothing but an explanation when the shell has no transport. */
  unavailable(reason: string): void {
    this.root.replaceChildren();
    const message = document.createElement('p');
    message.className = 'unavailable';
    message.textContent = reason;
    this.root.append(message);
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
    this.status.className = 'transport-status';
    this.track.className = 'transport-track';
    this.error.className = 'transport-error';

    this.position.type = 'range';
    this.position.min = '0';
    this.position.max = '1000';
    this.position.value = '0';
    this.position.className = 'transport-position';
    this.position.addEventListener('pointerdown', () => {
      this.scrubbing = true;
    });
    const commitSeek = (): void => {
      if (!this.scrubbing) {
        return;
      }
      this.scrubbing = false;
      const fraction = Number(this.position.value) / 1000;
      void this.call(() => sonora.player.seek({ positionMs: Math.round(fraction * this.duration) }));
    };
    this.position.addEventListener('pointerup', commitSeek);
    this.position.addEventListener('change', commitSeek);

    this.volume.type = 'range';
    this.volume.min = '0';
    this.volume.max = '100';
    this.volume.value = '100';
    this.volume.className = 'transport-volume';
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

    this.path.type = 'text';
    this.path.placeholder = 'C:\\Music\\track.flac';
    this.path.className = 'transport-path';
    const enqueue = (): void => {
      const value = this.path.value.trim();
      if (value === '') {
        return;
      }
      void this.call(async () => {
        await sonora.player.enqueue({ path: value });
        this.path.value = '';
      });
    };
    this.path.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') {
        enqueue();
      }
    });

    const controls = document.createElement('div');
    controls.className = 'transport-controls';
    controls.append(
      button('\u23EE', 'Previous', () => void this.call(() => sonora.player.previous())),
      this.playPause,
      button('\u23ED', 'Next', () => void this.call(() => sonora.player.next())),
      button('\u23F9', 'Stop', () => void this.call(() => sonora.player.stop())),
    );

    const scrubber = document.createElement('div');
    scrubber.className = 'transport-scrubber';
    scrubber.append(this.elapsed, this.position, this.remaining);

    const queue = document.createElement('div');
    queue.className = 'transport-queue';
    queue.append(this.path, button('Add', 'Add to the queue', enqueue));

    const volumeRow = document.createElement('label');
    volumeRow.className = 'transport-volume-row';
    volumeRow.append('Volume', this.volume);

    this.root.replaceChildren(
      this.status,
      this.track,
      scrubber,
      controls,
      volumeRow,
      queue,
      this.error,
    );
  }

  private duration = 0;

  private render(state: PlayerState): void {
    this.playing = state.state === 'playing';
    this.duration = state.durationMs;

    this.status.textContent =
      state.queueSize === 0
        ? 'Queue empty'
        : `${state.state} \u00B7 ${state.trackIndex + 1} of ${state.queueSize}` +
          (state.trackChanges > 0 ? ` \u00B7 ${state.trackChanges} gapless join(s)` : '') +
          (state.underruns > 0 ? ` \u00B7 ${state.underruns} underrun(s)` : '');

    this.track.textContent = state.currentPath === '' ? '\u2014' : state.currentPath;
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
