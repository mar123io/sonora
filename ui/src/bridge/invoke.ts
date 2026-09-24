import {
  createApi,
  type InvokeFn,
  type MethodName,
  type MethodSignatures,
  type SonoraApi,
} from './generated';

// The transport. Everything method-specific is generated; this file knows only
// how to get a string across and a string back.

interface SonoraQueryOptions {
  readonly request: string;
  readonly persistent?: boolean;
  readonly onSuccess: (response: string) => void;
  readonly onFailure: (errorCode: number, errorMessage: string) => void;
}

declare global {
  interface Window {
    /** Injected by the native side once the bridge router is wired up. */
    sonoraQuery?: (options: SonoraQueryOptions) => number;
    sonoraQueryCancel?: (requestId: number) => void;
    /** Exposed for the DevTools console. */
    sonora?: SonoraApi;
  }
}

/**
 * Codes 1-5 mirror sonora::bridge::ErrorCode and arrive from the native side.
 * Negative codes are ours and never cross the boundary.
 */
export const ErrorCode = {
  MalformedRequest: 1,
  UnknownMethod: 2,
  InvalidParams: 3,
  Unavailable: 4,
  InternalError: 5,

  /** The shell accepted the call and never answered. */
  Timeout: -2,
  /** There is no shell: the page is open in a plain browser. */
  NoBridge: -3,
  /** The shell answered with something that is not the expected JSON. */
  MalformedResponse: -4,
} as const;

export class BridgeError extends Error {
  constructor(
    readonly code: number,
    message: string,
    readonly method: string,
  ) {
    super(message);
    this.name = 'BridgeError';
  }
}

let timeoutMs = 10_000;

/**
 * A call that takes longer than this rejects and is cancelled natively. There
 * is no such thing as a call that just never comes back: that is how a UI ends
 * up with a spinner nobody can explain.
 */
export function setInvokeTimeout(ms: number): void {
  if (!Number.isFinite(ms) || ms <= 0) {
    throw new RangeError('timeout must be a positive number of milliseconds');
  }
  timeoutMs = ms;
}

export const invoke: InvokeFn = <M extends MethodName>(
  method: M,
  params: MethodSignatures[M]['params'],
): Promise<MethodSignatures[M]['result']> =>
  new Promise((resolve, reject) => {
    const query = window.sonoraQuery;
    if (typeof query !== 'function') {
      // Happens when index.html is opened in a normal browser, and it is worth
      // saying so plainly rather than hanging until the timeout.
      reject(
        new BridgeError(
          ErrorCode.NoBridge,
          'window.sonoraQuery is missing: this page is not running inside Sonora',
          method,
        ),
      );
      return;
    }

    let settled = false;
    let timer: ReturnType<typeof setTimeout> | undefined;

    const finish = (): void => {
      settled = true;
      if (timer !== undefined) {
        clearTimeout(timer);
      }
    };

    const requestId = query({
      request: JSON.stringify({ method, params }),
      persistent: false,
      onSuccess: (response) => {
        if (settled) {
          return;
        }
        finish();
        try {
          resolve(JSON.parse(response) as MethodSignatures[M]['result']);
        } catch {
          reject(
            new BridgeError(
              ErrorCode.MalformedResponse,
              'the shell returned something that is not JSON',
              method,
            ),
          );
        }
      },
      onFailure: (errorCode, errorMessage) => {
        if (settled) {
          return;
        }
        finish();
        reject(new BridgeError(errorCode, errorMessage, method));
      },
    });

    timer = setTimeout(() => {
      if (settled) {
        return;
      }
      settled = true;
      // Cancelling matters: without it the native side keeps a callback alive
      // for a promise nobody is waiting on any more.
      window.sonoraQueryCancel?.(requestId);
      reject(new BridgeError(ErrorCode.Timeout, `${method} did not answer in ${timeoutMs}ms`, method));
    }, timeoutMs);
  });

/** The typed API, built by the generated factory over the transport above. */
export const sonora: SonoraApi = createApi(invoke);

if (typeof window !== 'undefined') {
  // So `await sonora.shell.getVersion()` works in the console.
  window.sonora = sonora;
}
