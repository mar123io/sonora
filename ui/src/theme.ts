// Dark or light, and the small amount of state that needs.
//
// Dark is what Sonora is by default. Light is not a second stylesheet: it is the
// same rules with a different set of token values, so there is exactly one place
// where a colour decision lives. See the top of style.css.
//
// Three settings rather than two, because "follow the machine" is a real answer
// and the only one that is right on a laptop that switches at sunset. The
// stylesheet reads the absence of the attribute as that setting, which is why the
// system case removes it rather than writing "system" into it.

import { element, icon } from './format';

export type Theme = 'system' | 'dark' | 'light';

const kKey = 'sonora.theme';
const kOrder: readonly Theme[] = ['system', 'dark', 'light'];

function stored(): Theme {
  // localStorage on a custom scheme is not a given, and in some embeddings
  // reading it throws rather than returning null. A window that cannot remember
  // the setting is a small loss; a window that will not start because of it is
  // not.
  try {
    const saved = localStorage.getItem(kKey);
    return saved === 'dark' || saved === 'light' ? saved : 'system';
  } catch {
    return 'system';
  }
}

function remember(theme: Theme): void {
  try {
    if (theme === 'system') {
      localStorage.removeItem(kKey);
    } else {
      localStorage.setItem(kKey, theme);
    }
  } catch {
    // Then it lasts for this run. Said once, here, rather than checked at every
    // call site.
  }
}

let current: Theme = 'system';

function apply(theme: Theme): void {
  current = theme;
  if (theme === 'system') {
    document.documentElement.removeAttribute('data-theme');
  } else {
    document.documentElement.dataset['theme'] = theme;
  }
}

/** Called before the first paint, so the window never starts in the wrong theme. */
export function startTheme(): void {
  apply(stored());
}

/**
 * The control in the sidebar footer.
 *
 * It says which of the three settings is in force rather than which theme is on
 * screen, because those differ on a machine set to light and the person pressing
 * it is choosing between the settings.
 */
export function themeToggle(): HTMLButtonElement {
  const button = element('button', 'theme-toggle');
  button.type = 'button';
  const label = element('span');

  const draw = (): void => {
    label.textContent =
      current === 'system' ? 'Theme: system' : current === 'dark' ? 'Theme: dark' : 'Theme: light';
    button.title = 'Dark, light, or whatever this machine is set to';
  };

  button.append(icon('theme', 14), label);
  draw();

  button.addEventListener('click', () => {
    const next = kOrder[(kOrder.indexOf(current) + 1) % kOrder.length] ?? 'system';
    apply(next);
    remember(next);
    draw();
  });
  return button;
}
