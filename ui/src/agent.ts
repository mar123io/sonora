// The agent panel: a sentence in, and the plan the shell is willing to carry out.
//
// It shows everything, and that is the design rather than a debugging convenience. A plan the
// person cannot read is a plan they cannot agree to, so every step is listed with the tool's
// own name and the arguments the broker accepted -- and a refusal is shown in full, because a
// planner asking for something it may not have is the most interesting thing this system
// reports about itself. See ADR 0016.
//
// The panel holds no policy. It cannot: the catalogue, the effect classes and the refusals are
// all decided in sonora::agent, and this file would be the wrong place to re-implement any of
// them even if it tried. What it does is ask, display, and pass a plan id back.

import { CapabilitySet } from './bridge/capabilities';
import { type AgentStep, type AgentTool } from './bridge/generated';
import { sonora } from './bridge/invoke';
// The same helper the library and the queue use, and its comment is this panel's whole
// problem stated one layer down: textContent, never innerHTML, because every string here came
// out of a tag in somebody's music file. A plan is displayed with the same rule.
import { element } from './format';

// Pretty-prints the arguments the broker accepted. Falls back to the raw text, because a
// string that will not parse is still something the person should see rather than something
// this function should hide.
function readableArguments(json: string): string {
  if (json === '' || json === '{}') {
    return '';
  }
  try {
    return JSON.stringify(JSON.parse(json));
  } catch {
    return json;
  }
}

export class AgentPanel {
  private readonly input: HTMLInputElement;
  private readonly reply: HTMLParagraphElement;
  private readonly steps: HTMLOListElement;
  private readonly actions: HTMLDivElement;
  private readonly toolList: HTMLDetailsElement;
  private busy = false;
  private planId = '';

  constructor(
    private readonly host: HTMLElement,
    // What to do once a plan has run, so the queue and the transport catch up. The panel does
    // not touch them itself: it knows about the bridge and nothing about the rest of the page.
    private readonly onChanged: () => Promise<void>,
  ) {
    this.host.classList.add('agent');

    const form = element('form', 'agent-ask');
    this.input = element('input', 'agent-input');
    this.input.type = 'text';
    this.input.placeholder = 'play some bowie, pause, what is playing…';
    this.input.autocomplete = 'off';
    const send = element('button', 'agent-send', 'Ask');
    send.type = 'submit';
    form.append(this.input, send);
    form.addEventListener('submit', (event) => {
      event.preventDefault();
      void this.ask();
    });

    this.reply = element('p', 'agent-reply');
    this.steps = element('ol', 'agent-steps');
    this.actions = element('div', 'agent-actions');
    this.toolList = element('details', 'agent-tools');
    this.toolList.append(element('summary', undefined, 'What it is allowed to do'));

    this.host.append(form, this.reply, this.steps, this.actions, this.toolList);
  }

  async mount(capabilities: CapabilitySet): Promise<void> {
    if (!capabilities.has('agent')) {
      // Removed rather than disabled, for the same reason the diagnostics panel is: a text box
      // that refuses every sentence is worse than no text box.
      this.host.remove();
      return;
    }

    const described = await sonora.agent.describeTools();
    this.reply.textContent = `Ready. Interpreting with ${described.planner}.`;

    const table = element('table', 'agent-tool-table');
    for (const tool of described.tools) {
      table.append(this.describeTool(tool));
    }
    this.toolList.append(table);
    // Said here as well as in the ADR, because the panel is where somebody will wonder: the
    // list is short on purpose, and what is missing from it is missing by name.
    this.toolList.append(
      element(
        'p',
        'agent-note',
        'This is the whole list. Anything else the bridge can do is not hidden from the ' +
          'planner as a precaution — it is absent from the only list the shell will check a ' +
          'plan against.',
      ),
    );
  }

  private describeTool(tool: AgentTool): HTMLTableRowElement {
    const row = element('tr');
    row.append(element('td', 'agent-tool-name', tool.name));
    const effect = element('td', `agent-effect agent-effect-${tool.effect}`);
    effect.textContent = tool.effect === 'read' ? 'reads' : 'asks first';
    row.append(effect);
    row.append(element('td', 'agent-tool-summary', tool.description));
    return row;
  }

  private async ask(): Promise<void> {
    const utterance = this.input.value.trim();
    if (utterance === '' || this.busy) {
      return;
    }
    this.busy = true;
    this.clear();
    this.reply.textContent = 'Thinking…';

    try {
      const outcome = await sonora.agent.interpret({ utterance });
      this.reply.textContent = outcome.reply;
      this.render(outcome.performed, 'done');

      if (outcome.refusal !== '') {
        this.showRefusal(outcome.refusal, outcome.refusedTool);
        return;
      }
      if (outcome.planId === '') {
        this.planId = '';
        if (outcome.performed.length > 0) {
          await this.onChanged();
        }
        return;
      }

      this.planId = outcome.planId;
      this.render(outcome.pending, 'waiting');
      this.offer();
    } catch (error) {
      this.reply.textContent = `The shell refused that: ${String(error)}`;
    } finally {
      this.busy = false;
      this.input.select();
    }
  }

  private offer(): void {
    const accept = element('button', 'agent-accept', 'Do it');
    const decline = element('button', 'agent-decline', 'No');
    accept.addEventListener('click', () => void this.resolve(true));
    decline.addEventListener('click', () => void this.resolve(false));
    this.actions.append(accept, decline);
    accept.focus();
  }

  private async resolve(accept: boolean): Promise<void> {
    const planId = this.planId;
    if (planId === '' || this.busy) {
      return;
    }
    this.busy = true;
    this.planId = '';
    this.actions.replaceChildren();

    try {
      const outcome = await sonora.agent.resolve({ planId, accept });
      this.reply.textContent = outcome.reply;
      if (outcome.refusal !== '') {
        this.showRefusal(outcome.refusal, '');
        return;
      }
      this.render(outcome.performed, 'done', /*replace=*/ true);
      if (accept) {
        await this.onChanged();
      }
    } catch (error) {
      this.reply.textContent = `The shell refused that: ${String(error)}`;
    } finally {
      this.busy = false;
    }
  }

  private render(steps: readonly AgentStep[], state: 'done' | 'waiting', replace = false): void {
    if (replace) {
      this.steps.replaceChildren();
    }
    for (const step of steps) {
      const item = element('li', `agent-step agent-step-${state}`);
      item.append(element('code', 'agent-step-tool', step.tool));
      const args = readableArguments(step.argumentsJson);
      if (args !== '') {
        item.append(element('code', 'agent-step-args', args));
      }
      if (state === 'waiting') {
        item.append(element('span', 'agent-step-note', 'not run yet'));
      }
      this.steps.append(item);
    }
  }

  private showRefusal(refusal: string, tool: string): void {
    const box = element('p', 'agent-refusal');
    box.textContent = tool === '' ? `Refused: ${refusal}` : `Refused ${tool}: ${refusal}`;
    this.actions.append(box);
  }

  private clear(): void {
    this.planId = '';
    this.steps.replaceChildren();
    this.actions.replaceChildren();
  }
}
