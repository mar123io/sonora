import './style.css';

import { BridgeError, sonora } from './bridge/invoke';

// Week 3 replaces the browser-only diagnostics with the first real traffic over
// the bridge. Each row is a call that either proves something works or shows
// exactly how it failed -- including, deliberately, one call that must fail.

interface Row {
  readonly label: string;
  readonly value: string;
  readonly state: 'ok' | 'bad' | 'pending';
}

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

async function main(): Promise<void> {
  for (const label of ['Shell', 'Chromium', 'Capabilities', 'Round trip', 'Rejected call']) {
    set(label, 'calling...', 'pending');
  }

  try {
    const version = await sonora.shell.getVersion();
    set('Shell', `${version.version} (${version.gitDescribe})`, 'ok');
    set('Chromium', `${version.chromiumVersion} via CEF ${version.cefVersion}`, 'ok');
  } catch (error) {
    set('Shell', describe(error), 'bad');
    set('Chromium', 'unavailable', 'bad');
  }

  try {
    const capabilities = await sonora.shell.listCapabilities();
    const listed = capabilities.names.map(
      (name, index) => `${name}@${capabilities.versions[index]}`,
    );
    set('Capabilities', listed.join(', '), 'ok');
  } catch (error) {
    set('Capabilities', describe(error), 'bad');
  }

  try {
    const echoed = await sonora.shell.echo({ message: 'sonora', repeat: 2 });
    const expected = 'sonorasonora';
    const matches = echoed.message === expected && echoed.lengthBytes === expected.length;
    set('Round trip', matches ? `${echoed.message} (${echoed.lengthBytes} bytes)` : 'mismatch',
        matches ? 'ok' : 'bad');
  } catch (error) {
    set('Round trip', describe(error), 'bad');
  }

  try {
    // This one is SUPPOSED to fail. A bridge that only proves the happy path
    // has not proved that errors survive the crossing with their meaning
    // intact, and that is the half that matters when something breaks later.
    await sonora.shell.echo({ message: '' });
    set('Rejected call', 'the shell accepted an empty message', 'bad');
  } catch (error) {
    const expected = error instanceof BridgeError && error.code === 3;
    set('Rejected call', expected ? `rejected: ${error.message}` : describe(error),
        expected ? 'ok' : 'bad');
  }
}

void main();
