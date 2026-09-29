// The right-hand column, and the two things that live in it.
//
// The agent used to be a box floating over this column, positioned in fixed
// pixels from the bottom right and stacked above the diagnostics panel. Two
// problems, and only one of them was cosmetic: it covered the queue, which is the
// thing its plans change, so seeing what a plan had done meant moving the panel
// that had just done it. It is now the second tab of the column -- same width,
// same rules, no overlap, and the queue is one click away instead of underneath.
//
// This file owns only the strip and the two hosts. What goes in them is the
// queue panel's and the agent panel's business, which is why neither of them
// knows this file exists.

import { CapabilitySet } from './bridge/capabilities';
import { sonora } from './bridge/invoke';
import { element, formatCount, icon } from './format';
import { type QueueModel } from './queue';

interface Hosts {
  readonly queue: HTMLElement;
  readonly agent: HTMLElement;
}

/** The two elements outside this column that the narrow layout needs. */
interface Chrome {
  /** #app, which is what the drawer is positioned against and where the state lives. */
  readonly app: HTMLElement;
  /** #search, where the button that opens the drawer goes. */
  readonly header: HTMLElement;
}

export class SidePanel {
  private readonly queueTab = element('button', 'tab');
  private readonly askTab = element('button', 'tab', 'Ask');
  private readonly queueCount = element('span', 'tab-count');
  private readonly queueHost = element('div', 'tab-body');
  private readonly agentHost = element('div', 'tab-body');
  private readonly action = element('div', 'tab-action-slot');
  private readonly clear = element('button', 'tab-action', 'Clear');
  private readonly fresh = element('button', 'tab-action', 'New');
  private readonly toggle = element('button', 'side-toggle');
  private readonly scrim = element('div', 'scrim');
  private resetAsk: (() => void) | undefined;

  constructor(
    private readonly root: HTMLElement,
    private readonly model: QueueModel,
    private readonly chrome: Chrome,
  ) {}

  /** What the agent panel calls so that "New" can empty the conversation. */
  onAskReset(reset: () => void): void {
    this.resetAsk = reset;
  }

  mount(capabilities: CapabilitySet): Hosts {
    const tabs = element('div', 'tabs');
    tabs.setAttribute('role', 'tablist');

    this.queueTab.type = 'button';
    this.queueTab.setAttribute('role', 'tab');
    this.queueTab.append('Queue', ' ', this.queueCount);
    this.queueTab.addEventListener('click', () => this.select('queue'));

    this.askTab.type = 'button';
    this.askTab.setAttribute('role', 'tab');
    this.askTab.addEventListener('click', () => this.select('agent'));

    this.clear.type = 'button';
    this.clear.title = 'Empty the queue';
    this.clear.addEventListener('click', () => {
      void sonora.player.clearQueue();
    });

    this.fresh.type = 'button';
    this.fresh.title = 'Start a new conversation';
    this.fresh.addEventListener('click', () => this.resetAsk?.());

    tabs.append(this.queueTab);
    // A tab that refuses every sentence is worse than no tab, for the same reason
    // the diagnostics panel removes itself: see ADR 0016.
    if (capabilities.has('agent')) {
      tabs.append(this.askTab);
    }
    tabs.append(element('span', 'tabs-spacer'), this.action);

    for (const host of [this.queueHost, this.agentHost]) {
      host.setAttribute('role', 'tabpanel');
    }

    this.root.replaceChildren(tabs, this.queueHost, this.agentHost);
    this.select('queue');
    this.buildDrawer();

    this.model.onChange(() => this.renderCount());
    this.renderCount();

    return { queue: this.queueHost, agent: this.agentHost };
  }

  /**
   * The narrow layout, from this side.
   *
   * There is no width in this file and no matchMedia: the stylesheet decides at
   * what width the column becomes a drawer, and everything here is inert until
   * it does -- the button is display:none, the scrim is display:none, and
   * data-side is an attribute no rule reads. One breakpoint, written once, in
   * the file that owns layout.
   */
  private buildDrawer(): void {
    this.toggle.type = 'button';
    this.toggle.setAttribute('aria-controls', this.root.id);
    this.toggle.append(icon('albums', 14), 'Queue');
    this.toggle.addEventListener('click', () => this.setDrawer(!this.drawerOpen()));
    this.chrome.header.append(this.toggle);

    this.scrim.addEventListener('click', () => this.setDrawer(false));
    this.chrome.app.append(this.scrim);

    document.addEventListener('keydown', (event) => {
      if (event.key === 'Escape' && this.drawerOpen()) {
        this.setDrawer(false);
      }
    });
  }

  private drawerOpen(): boolean {
    return this.chrome.app.dataset['side'] === 'open';
  }

  private setDrawer(open: boolean): void {
    if (open) {
      this.chrome.app.dataset['side'] = 'open';
    } else {
      delete this.chrome.app.dataset['side'];
    }
    this.toggle.setAttribute('aria-expanded', String(open));
  }

  private select(which: 'queue' | 'agent'): void {
    const queue = which === 'queue';
    this.queueTab.dataset['active'] = String(queue);
    this.askTab.dataset['active'] = String(!queue);
    this.queueTab.setAttribute('aria-selected', String(queue));
    this.askTab.setAttribute('aria-selected', String(!queue));
    this.queueHost.hidden = !queue;
    this.agentHost.hidden = queue;
    // The action belongs to whichever tab is showing: they do different things
    // and only one of them applies at a time.
    this.action.replaceChildren(queue ? this.clear : this.fresh);
  }

  private renderCount(): void {
    const count = this.model.all.length;
    this.queueCount.textContent = count === 0 ? '' : formatCount(count);
    this.clear.disabled = count === 0;
  }
}
