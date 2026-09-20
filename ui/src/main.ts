import './style.css';

// Week 2 has no bridge yet, so this page can only report what the browser
// itself knows. Every line here is a check on something the shell is supposed
// to have set up: the custom scheme, its origin, and whether Chromium decided
// the origin is trustworthy.
//
// Week 3 replaces this with the first real bridge call.

interface Diagnostic {
  readonly label: string;
  readonly value: string;
  readonly ok: boolean;
}

function collect(): Diagnostic[] {
  const url = new URL(window.location.href);

  return [
    {
      label: 'Origin',
      value: window.location.origin,
      ok: window.location.origin === 'sonora://app',
    },
    {
      label: 'Scheme',
      value: url.protocol.replace(':', ''),
      ok: url.protocol === 'sonora:',
    },
    {
      label: 'Secure context',
      value: String(window.isSecureContext),
      // CEF_SCHEME_OPTION_SECURE is what makes this true. If it is false the
      // scheme was registered without it, and most modern web APIs will refuse
      // to run later on.
      ok: window.isSecureContext,
    },
    {
      label: 'Storage',
      value: storageAvailable() ? 'available' : 'unavailable',
      // Requires CEF_SCHEME_OPTION_STANDARD: a non-standard scheme has an
      // opaque origin and no storage.
      ok: storageAvailable(),
    },
    {
      label: 'Device pixel ratio',
      value: String(window.devicePixelRatio),
      ok: true,
    },
  ];
}

function storageAvailable(): boolean {
  try {
    const probe = '__sonora__';
    window.localStorage.setItem(probe, probe);
    window.localStorage.removeItem(probe);
    return true;
  } catch {
    return false;
  }
}

function render(diagnostics: Diagnostic[]): void {
  const list = document.querySelector<HTMLDListElement>('#diagnostics');
  if (list === null) {
    return;
  }
  list.replaceChildren(
    ...diagnostics.flatMap((entry) => {
      const term = document.createElement('dt');
      term.textContent = entry.label;

      const value = document.createElement('dd');
      value.textContent = entry.value;
      value.dataset['state'] = entry.ok ? 'ok' : 'bad';

      return [term, value];
    }),
  );
}

render(collect());
