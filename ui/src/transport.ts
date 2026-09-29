import { CapabilitySet } from './bridge/capabilities';
import { onEvent } from './bridge/events';
import { BridgeError, sonora } from './bridge/invoke';
import { type PlayerState } from './bridge/generated';
import { coverElement, element, formatTime, icon, type IconName } from './format';
import { type QueueModel } from './queue';

// The transport: the part of the interface that is about sound rather than about
// the bridge.
//
// It never asks the shell what the state is on a timer. The shell says so, four
// times a second, through player.state -- and the whole state each time rather
// than a delta, so a page that missed an event is out of date for 250 ms instead
// of wrong until the next reload.
//
// Two values are deliberately not driven by that event while the user is touching
// them: dragging the position bar or the volume would otherwise fight the incoming
// state, and the control would jump under the finger.
//
// It is one row, 72px, and it never changes height. It used to be three stacked
// rows -- now-playing, then controls over a scrubber, then a status line and an
// error line that were empty most of the time and reserved their space anyway --
// which took 150px off the track list to say less. What the status line used to
// print (which track of how many, the gapless joins, the underruns) is now the
// tooltip on this bar: real numbers, still one hover away, no longer occupying the
// window. The error is the exception and is still drawn, above the bar, because an
// error is the reason there is silence.

function describe(error: unknown): string {
  if (error instanceof BridgeError) {
    return `${error.message} (code ${error.code})`;
  }
  return error instanceof Error ? error.message : String(error);
}

function iconButton(
  name: IconName,
  label: string,
  className: string,
  size: number,
  onClick: () => void,
): HTMLButtonElement {
  const button = element('button', `transport-button ${className}`);
  button.type = 'button';
  // An icon with no text needs the name said somewhere a screen reader can reach,
  // and a tooltip is not that place.
  button.setAttribute('aria-label', label);
  button.title = label;
  button.append(icon(name, size));
  button.addEventListener('click', onClick);
  return button;
}

export class Transport {
  private readonly nowPlaying = element('div', 'transport-now');
  private readonly elapsed = element('span', 'transport-elapsed', '--:--');
  private readonly total = element('span', 'transport-total', '--:--');
  private readonly position = element('input', 'range transport-position');
  private readonly volume = element('input', 'range transport-volume');
  private readonly playPause = element('button', 'transport-button transport-play');
  private readonly mute = element('button', 'transport-button transport-mute');
  private readonly previous: HTMLButtonElement;
  private readonly next: HTMLButtonElement;
  private readonly stop: HTMLButtonElement;
  private readonly error = element('p', 'transport-error');

  private scrubbing = false;
  private adjustingVolume = false;
  private playing = false;
  private loaded = false;
  private duration = 0;
  /** So that unmuting goes back to where the level was, not to full. */
  private lastAudible = 1;

  constructor(
    private readonly root: HTMLElement,
    private readonly queue: QueueModel,
  ) {
    this.previous = iconButton('previous', 'Previous', '', 17, () =>
      void this.call(() => sonora.player.previous()),
    );
    this.next = iconButton('next', 'Next', '', 17, () => void this.call(() => sonora.player.next()));
    this.stop = iconButton('stop', 'Stop', 'transport-stop', 15, () =>
      void this.call(() => sonora.player.stop()),
    );
  }

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
    // The title and the cover come from the queue, not from the state event: the
    // state knows an index, the queue knows what is at it.
    this.queue.onChange(() => this.renderNowPlaying());

    // The first paint does not wait for an event: at four a second the first one
    // is up to 250 ms away, and a transport that appears blank and then fills in
    // looks broken rather than fast.
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
    this.position.setAttribute('aria-label', 'Position in the track');
    this.position.addEventListener('pointerdown', () => {
      this.scrubbing = true;
    });
    this.position.addEventListener('input', () => {
      // The filled part follows the thumb while it is dragged. Without this the
      // bar only catches up when the seek lands, which reads as lag in the
      // control rather than in the seek.
      this.position.style.setProperty('--fill', String(Number(this.position.value) / 1000));
      this.elapsed.textContent = formatTime((Number(this.position.value) / 1000) * this.duration);
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
    this.volume.setAttribute('aria-label', 'Volume');
    this.volume.addEventListener('pointerdown', () => {
      this.adjustingVolume = true;
    });
    this.volume.addEventListener('pointerup', () => {
      this.adjustingVolume = false;
    });
    this.volume.addEventListener('input', () => {
      const level = Number(this.volume.value) / 100;
      this.volume.style.setProperty('--fill', String(level));
      if (level > 0) {
        this.lastAudible = level;
      }
      this.drawMute(level);
      void this.call(() => sonora.player.setVolume({ level }));
    });

    this.playPause.type = 'button';
    this.playPause.addEventListener('click', () => {
      void this.call(() => (this.playing ? sonora.player.pause() : sonora.player.play()));
    });
    this.drawPlayPause();

    this.mute.type = 'button';
    this.mute.classList.add('transport-mute');
    this.mute.addEventListener('click', () => {
      const level = Number(this.volume.value) / 100 > 0 ? 0 : this.lastAudible;
      this.volume.value = String(Math.round(level * 100));
      this.volume.style.setProperty('--fill', String(level));
      this.drawMute(level);
      void this.call(() => sonora.player.setVolume({ level }));
    });
    this.drawMute(1);

    const controls = element('div', 'transport-controls');
    controls.append(this.previous, this.playPause, this.next, this.stop);

    const scrubber = element('div', 'transport-scrubber');
    scrubber.append(this.elapsed, this.position, this.total);

    const volumeRow = element('div', 'transport-volume-row');
    volumeRow.append(this.mute, this.volume);

    this.error.hidden = true;
    this.root.replaceChildren(this.nowPlaying, controls, scrubber, volumeRow, this.error);
    this.renderNowPlaying();
  }

  private drawPlayPause(): void {
    const label = this.playing ? 'Pause' : 'Play';
    this.playPause.setAttribute('aria-label', label);
    this.playPause.title = label;
    this.playPause.replaceChildren(icon(this.playing ? 'pause' : 'play', 16));
  }

  private drawMute(level: number): void {
    const muted = level <= 0;
    const label = muted ? 'Unmute' : 'Mute';
    this.mute.setAttribute('aria-label', label);
    this.mute.title = label;
    this.mute.replaceChildren(icon(muted ? 'muted' : 'volume', 16));
  }

  private renderNowPlaying(): void {
    const entry = this.queue.current;
    if (entry === undefined) {
      this.nowPlaying.replaceChildren(
        coverElement('', '', 'cover cover-md'),
        element('span', 'transport-idle', 'Nothing playing'),
      );
      return;
    }
    const text = element('div', 'transport-labels');
    text.append(
      element('span', 'transport-title', entry.title),
      element(
        'span',
        'transport-artist',
        // Artist and album on one line, because the row is 72px and two lines of
        // metadata under a title is what made it 96.
        [entry.artist, entry.album].filter((part) => part !== '').join(' · '),
      ),
    );
    this.nowPlaying.replaceChildren(
      coverElement(entry.artUrl, entry.album || entry.title, 'cover cover-md'),
      text,
    );
  }

  private render(state: PlayerState): void {
    this.playing = state.state === 'playing';
    this.loaded = state.trackIndex >= 0 && state.queueSize > 0;
    this.duration = state.durationMs;

    this.root.dataset['state'] = this.loaded ? state.state : 'idle';
    this.drawPlayPause();

    // What the status line used to say, now where it does not cost a row. The
    // underruns and the gapless joins are the two numbers in this interface that
    // are about the audio thread rather than about the music, and they are worth
    // keeping reachable.
    this.root.title =
      state.queueSize === 0
        ? 'Nothing queued'
        : `${state.state} · ${state.trackIndex + 1} of ${state.queueSize}` +
          (state.trackChanges > 0 ? ` · ${state.trackChanges} gapless join(s)` : '') +
          (state.underruns > 0 ? ` · ${state.underruns} underrun(s)` : '');

    for (const control of [this.previous, this.next, this.stop]) {
      control.disabled = !this.loaded;
    }
    this.playPause.disabled = state.queueSize === 0;
    this.position.disabled = !this.loaded || state.durationMs <= 0;

    this.elapsed.textContent = this.loaded ? formatTime(state.positionMs) : '--:--';
    this.total.textContent = this.loaded ? formatTime(state.durationMs) : '--:--';

    // Not while the user is holding the control: an incoming state would drag the
    // thumb back to where playback actually is, under the finger.
    if (!this.scrubbing) {
      const fraction = state.durationMs > 0 ? state.positionMs / state.durationMs : 0;
      this.position.value = String(Math.round(fraction * 1000));
      this.position.style.setProperty('--fill', String(fraction));
    }
    if (!this.adjustingVolume) {
      this.volume.value = String(Math.round(state.volume * 100));
      this.volume.style.setProperty('--fill', String(state.volume));
      if (state.volume > 0) {
        this.lastAudible = state.volume;
      }
      this.drawMute(state.volume);
    }

    this.fail(state.lastError);
  }

  private fail(message: string): void {
    this.error.textContent = message;
    this.error.hidden = message === '';
  }

  private async call(action: () => Promise<unknown>): Promise<void> {
    try {
      await action();
      this.fail('');
    } catch (caught) {
      this.fail(describe(caught));
    }
  }
}
