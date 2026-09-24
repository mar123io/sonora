import { type EventName, type EventPayloads } from './generated';

// The other direction.
//
// A query is the page asking a question and waiting for an answer. An event is
// the shell saying something nobody asked for -- playback moved, a download
// progressed, the network came back. The two need different machinery, and
// pretending otherwise (a long-lived query that answers many times) ties every
// event to a call the page happened to make and ends the stream silently on the
// next reload.
//
// The native side calls one function with a batch, at most a few times a
// second. What arrives here is already rate-limited: see
// src/bridge/event_coalescer.h.

type Handler<E extends EventName> = (payload: EventPayloads[E]) => void;

interface Envelope {
  readonly name: string;
  readonly payload: unknown;
}

declare global {
  interface Window {
    /** Called by the shell. The argument is a JSON string, never JavaScript. */
    __sonoraEvents?: (batch: string) => void;
  }
}

// Set<Handler<EventName>> would be a lie: each set holds handlers for one
// specific event, and the map key is what says which. The cast is confined to
// deliver() below, where the key and the payload come from the same envelope.
const handlers = new Map<string, Set<Handler<never>>>();
const unknownEvents = new Set<string>();

/**
 * Subscribes to an event. Returns the unsubscribe function; a component that
 * does not call it keeps its handler alive for the life of the page.
 */
export function onEvent<E extends EventName>(name: E, handler: Handler<E>): () => void {
  let set = handlers.get(name);
  if (set === undefined) {
    set = new Set();
    handlers.set(name, set);
  }
  set.add(handler as Handler<never>);

  return () => {
    handlers.get(name)?.delete(handler as Handler<never>);
  };
}

function deliver(envelope: Envelope): void {
  const set = handlers.get(envelope.name);
  if (set === undefined || set.size === 0) {
    // A shell newer than this bundle can send events this build has never
    // heard of. That is not an error -- it is the whole point of versioning
    // the protocol -- but it should be sayable, once, rather than silent.
    if (!unknownEvents.has(envelope.name)) {
      unknownEvents.add(envelope.name);
      console.debug(`sonora: no handler for event '${envelope.name}'`);
    }
    return;
  }

  for (const handler of [...set]) {
    try {
      (handler as (payload: unknown) => void)(envelope.payload);
    } catch (error) {
      // One bad subscriber must not stop the others, and must not throw back
      // into the native side: this function is called from C++.
      console.error(`sonora: handler for '${envelope.name}' threw`, error);
    }
  }
}

function receive(batch: string): void {
  let parsed: unknown;
  try {
    parsed = JSON.parse(batch);
  } catch (error) {
    console.error('sonora: event batch was not JSON', error);
    return;
  }
  if (!Array.isArray(parsed)) {
    console.error('sonora: event batch was not an array');
    return;
  }
  for (const envelope of parsed as Envelope[]) {
    if (typeof envelope?.name === 'string') {
      deliver(envelope);
    }
  }
}

if (typeof window !== 'undefined') {
  window.__sonoraEvents = receive;
}
