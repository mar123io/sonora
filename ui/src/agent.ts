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
//
// It keeps the conversation rather than only the last exchange. That is not decoration: the
// sentence that was misread is the evidence for why the answer is wrong, and a panel that
// overwrites it leaves the person retyping it to find out. `play some bastille` was found this
// way, and with the old panel the only trace of it was a screenshot.

import { CapabilitySet } from './bridge/capabilities';
import { type AgentStep, type AgentTool } from './bridge/generated';
import { sonora } from './bridge/invoke';
// The same helper the library and the queue use, and its comment is this panel's whole
// problem stated one layer down: textContent, never innerHTML, because every string here came
// out of a tag in somebody's music file. A plan is displayed with the same rule.
import { element, icon, plural } from './format';

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
  private readonly chat = element('div', 'chat');
  private readonly input = element('input', 'composer-input');
  private readonly send = element('button', 'composer-send');
  private readonly form = element('form', 'composer-field');
  private planner = '';
  private tools: readonly AgentTool[] = [];
  private busy = false;
  private planId = '';

  constructor(
    private readonly host: HTMLElement,
    // What to do once a plan has run, so the queue and the transport catch up. The panel does
    // not touch them itself: it knows about the bridge and nothing about the rest of the page.
    private readonly onChanged: () => Promise<void>,
  ) {}

  async mount(capabilities: CapabilitySet): Promise<void> {
    if (!capabilities.has('agent')) {
      // Emptied rather than left with a disabled box in it, for the same reason the
      // diagnostics panel removes itself: a text field that refuses every sentence is worse
      // than no text field. The tab that would have held this is not drawn either.
      this.host.replaceChildren();
      return;
    }

    this.build();

    const described = await sonora.agent.describeTools();
    this.planner = described.planner;
    this.tools = described.tools;
    this.reset();
  }

  /** Empties the conversation and says hello again. Wired to "New" in the tab strip. */
  reset(): void {
    this.planId = '';
    this.chat.replaceChildren();
    const opening = this.shellTurn();
    opening.append(
      element('p', 'turn-reply', `Ready. Interpreting with ${this.planner}.`),
      this.toolList(),
    );
    this.input.focus();
  }

  private build(): void {
    this.input.type = 'text';
    this.input.placeholder = 'Ask Sonora to play something';
    this.input.autocomplete = 'off';
    this.input.id = 'agent-input';

    const label = element('label', 'sr-only', 'Ask Sonora');
    label.htmlFor = this.input.id;

    this.send.type = 'submit';
    this.send.setAttribute('aria-label', 'Send');
    this.send.title = 'Send';
    this.send.append(icon('send', 14));

    this.form.append(label, this.input, this.send);
    this.form.addEventListener('submit', (event) => {
      event.preventDefault();
      void this.ask();
    });

    const composer = element('div', 'composer');
    composer.append(
      this.form,
      element(
        'p',
        'composer-note',
        'Runs locally. Reads freely; asks before it changes the queue.',
      ),
    );

    this.host.replaceChildren(this.chat, composer);
  }

  /** The list of what it may do, folded away but never further than one click. */
  private toolList(): HTMLDetailsElement {
    const details = element('details', 'tools');
    details.append(element('summary', undefined, `What it is allowed to do (${this.tools.length})`));

    const table = element('table', 'tool-table');
    for (const tool of this.tools) {
      const row = element('tr');
      row.append(element('td', 'tool-name', tool.name));
      const effect = element('td', `tool-effect tool-effect-${tool.effect}`);
      effect.textContent = tool.effect === 'read' ? 'reads' : 'asks first';
      row.append(effect, element('td', 'tool-summary', tool.description));
      table.append(row);
    }
    details.append(table);

    // Said here as well as in the ADR, because the panel is where somebody will wonder: the
    // list is short on purpose, and what is missing from it is missing by name.
    details.append(
      element(
        'p',
        'composer-note',
        'This is the whole list. Anything else the bridge can do is not hidden from the ' +
          'planner as a precaution — it is absent from the only list the shell will check a ' +
          'plan against.',
      ),
    );
    return details;
  }

  private shellTurn(): HTMLElement {
    const turn = element('div', 'turn-shell');
    this.chat.append(turn);
    this.toBottom();
    return turn;
  }

  private toBottom(): void {
    // After the layout this append caused, not before it.
    requestAnimationFrame(() => {
      this.chat.scrollTop = this.chat.scrollHeight;
    });
  }

  private async ask(): Promise<void> {
    const utterance = this.input.value.trim();
    if (utterance === '' || this.busy) {
      return;
    }
    this.busy = true;
    this.send.disabled = true;
    this.planId = '';
    this.input.value = '';

    this.chat.append(element('div', 'turn-you', utterance));
    const turn = this.shellTurn();
    const reply = element('p', 'turn-reply', 'Thinking…');
    reply.dataset['thinking'] = 'true';
    turn.append(reply);

    try {
      const outcome = await sonora.agent.interpret({ utterance });
      reply.textContent = outcome.reply;
      delete reply.dataset['thinking'];

      if (outcome.performed.length > 0) {
        turn.append(this.stepList(outcome.performed));
      }

      if (outcome.refusal !== '') {
        turn.append(this.refusal(outcome.refusal, outcome.refusedTool));
      } else if (outcome.planId === '') {
        if (outcome.performed.length > 0) {
          await this.onChanged();
        }
      } else {
        this.planId = outcome.planId;
        turn.append(this.planCard(outcome.pending));
      }
    } catch (error) {
      reply.textContent = `The shell refused that: ${String(error)}`;
      delete reply.dataset['thinking'];
      reply.classList.add('refusal');
    } finally {
      this.busy = false;
      this.send.disabled = false;
      this.input.focus();
      this.toBottom();
    }
  }

  /** What already ran, with a tick each: reads are the cheap half and cannot change anything. */
  private stepList(steps: readonly AgentStep[]): HTMLElement {
    const list = element('ul', 'steps');
    for (const step of steps) {
      const item = element('li', 'step step-done');
      item.append(icon('done', 13), element('code', 'step-tool', step.tool));
      const args = readableArguments(step.argumentsJson);
      if (args !== '') {
        item.append(element('span', 'step-args', args));
      }
      if (step.resultJson !== '') {
        // The whole answer, in the tooltip. In the line it would be a wall of JSON; hidden
        // entirely it would be a step whose effect nobody can check.
        item.title = step.resultJson;
      }
      list.append(item);
    }
    return list;
  }

  private planCard(pending: readonly AgentStep[]): HTMLElement {
    const card = element('div', 'plan');

    const head = element('div', 'plan-head');
    head.append(
      icon('warning', 15),
      element(
        'span',
        undefined,
        `${plural(pending.length, 'step')} ${pending.length === 1 ? 'needs' : 'need'} your go-ahead`,
      ),
    );

    const steps = element('ol', 'plan-steps');
    for (const step of pending) {
      const item = element('li');
      item.append(element('code', 'step-tool', step.tool));
      const args = readableArguments(step.argumentsJson);
      if (args !== '') {
        item.append(' ', element('span', 'step-args', args));
      }
      steps.append(item);
    }

    const accept = element('button', 'button button-primary', 'Do it');
    accept.type = 'button';
    const decline = element('button', 'button', 'Cancel');
    decline.type = 'button';

    const actions = element('div', 'plan-actions');
    actions.append(accept, decline);

    const note = element(
      'p',
      'composer-note',
      'Accepted whole or not at all. Nothing in this list has happened yet.',
    );

    card.append(head, steps, actions, note);

    const answer = (accepted: boolean): void => {
      actions.replaceChildren();
      void this.resolve(accepted, card);
    };
    accept.addEventListener('click', () => answer(true));
    decline.addEventListener('click', () => answer(false));
    accept.focus();
    return card;
  }

  private async resolve(accept: boolean, card: HTMLElement): Promise<void> {
    const planId = this.planId;
    if (planId === '' || this.busy) {
      return;
    }
    this.busy = true;
    this.planId = '';

    const turn = this.shellTurn();
    try {
      const outcome = await sonora.agent.resolve({ planId, accept });
      // The card stops being a question once it has been answered: the steps stay, the
      // buttons are gone, and the border loses the colour that meant "waiting".
      card.classList.add('plan-answered');
      turn.append(element('p', 'turn-reply', outcome.reply));
      if (outcome.refusal !== '') {
        turn.append(this.refusal(outcome.refusal, ''));
        return;
      }
      if (outcome.performed.length > 0) {
        turn.append(this.stepList(outcome.performed));
      }
      if (accept) {
        await this.onChanged();
      }
    } catch (error) {
      turn.append(this.refusal(String(error), ''));
    } finally {
      this.busy = false;
      this.input.focus();
      this.toBottom();
    }
  }

  private refusal(refusal: string, tool: string): HTMLElement {
    return element(
      'p',
      'refusal',
      tool === '' ? `Refused: ${refusal}` : `Refused ${tool}: ${refusal}`,
    );
  }
}
