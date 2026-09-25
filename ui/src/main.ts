import './style.css';

import { CapabilitySet } from './bridge/capabilities';
import { onEvent } from './bridge/events';
import { BridgeError, sonora } from './bridge/invoke';
import { LibraryView } from './library';
import { QueueModel, QueuePanel } from './queue';
import { Transport } from './transport';

// Week 7 turns the page into a music player: a library on the left, a list in
// the middle, a queue on the right and the transport along the bottom.
//
// The diagnostics that were the whole page in week 4 are still here, in a panel
// that starts closed. They have not stopped being useful -- they are how the
// bridge, the capability negotiation and the event coalescing are checked from
// the inside -- they are just no longer what the window is for.

interface Row {
  readonly label: string;
  readonly value: string;
  readonly state: 'ok' | 'bad' | 'degraded' | 'pending';
}

const LABELS = [
  'Shell',
  'Chromium',
  'Protocol',
  'Capabilities',
  'Round trip',
  'Rejected call',
  'Diagnostics',
  'Heartbeat',
] as const;

const rows = new Map<string, Row>();

function set(label: string, value: string, state: Row['state']): void {
  rows.set(label, { label, value, state });
  render();
}

function render(): void {
  const list = document.querySelector<HTMLDListElement>('#diagnostics');
  if (list === null) {
    return;
  }
  list.replaceChildren(
    ...[...rows.values()].flatMap((row) => {
      const term = document.createElement('dt');
      term.textContent = row.label;

      const value = document.createElement('dd');
      value.textContent = row.value;
      value.dataset['state'] = row.state;

      return [term, value];
    }),
  );
}

function describe(error: unknown): string {
  if (error instanceof BridgeError) {
    return `${error.message} (code ${error.code})`;
  }
  return error instanceof Error ? error.message : String(error);
}

async function negotiate(): Promise<CapabilitySet> {
  // The first call, before anything else is attempted. Everything below reads
  // the answer instead of assuming one.
  try {
    const capabilities = CapabilitySet.from(await sonora.shell.getCapabilities());

    set(
      'Protocol',
      capabilities.protocolMatches
        ? `v${capabilities.shellProtocolVersion}`
        : `shell speaks v${capabilities.shellProtocolVersion}, this page speaks another`,
      capabilities.protocolMatches ? 'ok' : 'bad',
    );

    const listed = capabilities.all
      .map((c) => `${c.name}@${c.version}${c.enabled ? '' : ' (off)'}`)
      .join(', ');
    const unmet = capabilities.unmet;
    set(
      'Capabilities',
      unmet.length === 0 ? listed : `${listed} — missing ${unmet.map((c) => c.name).join(', ')}`,
      unmet.length === 0 ? 'ok' : 'degraded',
    );

    return capabilities;
  } catch (error) {
    set('Protocol', describe(error), 'bad');
    set('Capabilities', 'unavailable', 'bad');
    return CapabilitySet.empty();
  }
}

async function checkVersion(): Promise<void> {
  try {
    const version = await sonora.shell.getVersion();
    set('Shell', `${version.version} (${version.gitDescribe})`, 'ok');
    set('Chromium', `${version.chromiumVersion} via CEF ${version.cefVersion}`, 'ok');
  } catch (error) {
    set('Shell', describe(error), 'bad');
    set('Chromium', 'unavailable', 'bad');
  }
}

async function checkRoundTrip(): Promise<void> {
  try {
    const echoed = await sonora.shell.echo({ message: 'sonora', repeat: 2 });
    const expected = 'sonorasonora';
    const matches = echoed.message === expected && echoed.lengthBytes === expected.length;
    set(
      'Round trip',
      matches ? `${echoed.message} (${echoed.lengthBytes} bytes)` : 'mismatch',
      matches ? 'ok' : 'bad',
    );
  } catch (error) {
    set('Round trip', describe(error), 'bad');
  }
}

async function checkRejection(): Promise<void> {
  try {
    // This one is SUPPOSED to fail. A bridge that only proves the happy path
    // has not proved that errors survive the crossing with their meaning
    // intact, and that is the half that matters when something breaks later.
    await sonora.shell.echo({ message: '' });
    set('Rejected call', 'the shell accepted an empty message', 'bad');
  } catch (error) {
    const expected = error instanceof BridgeError && error.code === 3;
    set(
      'Rejected call',
      expected ? `rejected: ${error.message}` : describe(error),
      expected ? 'ok' : 'bad',
    );
  }
}

async function checkDiagnostics(capabilities: CapabilitySet): Promise<void> {
  if (!capabilities.has('diagnostics')) {
    // The degraded path, and the reason SONORA_DISABLE_CAPS exists: this line
    // runs on a normal build, on demand, instead of only against a shell from
    // six months ago that nobody has to hand.
    set(
      'Diagnostics',
      capabilities.isDisabled('diagnostics')
        ? 'switched off for this run — hidden, not broken'
        : 'not offered by this shell',
      'degraded',
    );
    set('Heartbeat', 'not subscribed', 'degraded');
    return;
  }

  try {
    const metrics = await sonora.diagnostics.getMetrics();
    set(
      'Diagnostics',
      `up ${(metrics.uptimeMs / 1000).toFixed(1)}s · ${metrics.queriesHandled} queries · ` +
        `${metrics.eventsDelivered} of ${metrics.eventsPosted} events delivered`,
      'ok',
    );
  } catch (error) {
    set('Diagnostics', describe(error), 'bad');
  }
}

function subscribeToHeartbeat(capabilities: CapabilitySet): void {
  if (!capabilities.has('diagnostics')) {
    return;
  }

  let received = 0;
  set('Heartbeat', 'waiting...', 'pending');

  onEvent('diagnostics.heartbeat', (payload) => {
    received += 1;
    // sequence counts what the shell produced, received counts what arrived.
    // The gap is the coalescing, and it is the whole point: the native side
    // emits at 20 Hz and the page is told at 4 Hz, without either end
    // negotiating it per call.
    const ratio = payload.sequence > 0 ? Math.round((received / payload.sequence) * 100) : 0;
    set(
      'Heartbeat',
      `#${payload.sequence} · ${received} of ${payload.sequence} arrived (${ratio}%)`,
      'ok',
    );
  });
}

function root(selector: string): HTMLElement {
  const found = document.querySelector<HTMLElement>(selector);
  if (found === null) {
    // index.html and this file are one artefact, built together. A missing mount
    // point is a build that is wrong, not a case to handle.
    throw new Error(`sonora: ${selector} is missing from the page`);
  }
  return found;
}

async function main(): Promise<void> {
  for (const label of LABELS) {
    set(label, 'calling...', 'pending');
  }

  const capabilities = await negotiate();

  // The player first: it is what the window is for, and it should not wait
  // behind four calls that are about the bridge.
  const queue = new QueueModel();
  queue.enable(capabilities);

  const transport = new Transport(root('#transport'), queue);
  void transport.mount(capabilities);
  new QueuePanel(root('#queue'), queue).mount(capabilities);

  const library = new LibraryView(
    { sidebar: root('#sidebar'), content: root('#content'), search: root('#search') },
    {
      // Replace and play: picking a track in the library means "play this now",
      // and one call does both so there is no moment where the queue holds the
      // new tracks and the player is still on the old one.
      play: async (trackIds) => {
        if (trackIds.length === 0) {
          return;
        }
        await sonora.player.enqueue({ trackIds: [...trackIds], replace: true });
        await sonora.player.play();
        await queue.refresh();
      },
      enqueue: async (trackIds) => {
        if (trackIds.length === 0) {
          return;
        }
        await sonora.player.enqueue({ trackIds: [...trackIds] });
        await queue.refresh();
      },
    },
  );
  void library.mount(capabilities);

  // One subscription, fanned out here rather than three components each asking
  // the shell for the same thing.
  onEvent('player.state', (payload) => queue.observe(payload.player));
  onEvent('library.status', (payload) => library.onStatus(payload.library));
  void queue.refresh();

  await checkVersion();
  await checkRoundTrip();
  await checkRejection();
  await checkDiagnostics(capabilities);
  subscribeToHeartbeat(capabilities);
}

void main();
