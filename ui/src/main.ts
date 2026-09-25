import './style.css';

import { CapabilitySet } from './bridge/capabilities';
import { onEvent } from './bridge/events';
import { BridgeError, sonora } from './bridge/invoke';
import { Transport } from './transport';

// Week 4 turns the diagnostics page into the thing it was always going to be:
// a page that asks the shell what it can do, uses what is there, and says so
// plainly about what is not. The last two rows are the interesting ones -- one
// shows a capability that may be switched off, the other shows events arriving
// at a rate the page never asked for and the shell decided.

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

async function main(): Promise<void> {
  for (const label of LABELS) {
    set(label, 'calling...', 'pending');
  }

  const capabilities = await negotiate();

  // Mounted before the diagnostics finish: the transport is what the window is
  // for, and it should not wait behind four calls that are about the bridge.
  const transportRoot = document.querySelector<HTMLElement>('#transport');
  if (transportRoot !== null) {
    void new Transport(transportRoot).mount(capabilities);
  }

  await checkVersion();
  await checkRoundTrip();
  await checkRejection();
  await checkDiagnostics(capabilities);
  subscribeToHeartbeat(capabilities);
}

void main();
