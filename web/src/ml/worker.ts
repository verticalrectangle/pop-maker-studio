/**
 * Shared worker RPC helpers.
 *
 * Two sides:
 *  - `WorkerRPC` (main thread): wraps a `Worker`, exposes promise-based
 *    `call(method, args, transfer?, onProgress?)` with id-based request/response
 *    and a progress callback channel.
 *  - `serve(methods)` (worker thread): registers method handlers, wires
 *    `self.onmessage`, posts responses / progress / errors back.
 *
 * Wire protocol (all messages are plain objects posted via postMessage):
 *   request:   { type: 'req',      id, method, args }
 *   response:  { type: 'res',      id, result }
 *   error:     { type: 'err',      id, error: string }
 *   progress:  { type: 'progress', id, p: number, msg?: string }
 *
 * A handler may return a plain value (structured-cloned back) or a
 * `{ result, transfer }` shape to transfer specific `Transferable`s back.
 */

export type ProgressCb = (p: number, msg?: string) => void;

interface Pending {
  resolve: (value: unknown) => void;
  reject: (reason: unknown) => void;
  onProgress?: ProgressCb;
}

interface ReqMessage {
  type: 'req';
  id: number;
  method: string;
  args: unknown;
}
interface ResMessage {
  type: 'res';
  id: number;
  result: unknown;
}
interface ErrMessage {
  type: 'err';
  id: number;
  error: string;
}
interface ProgressMessage {
  type: 'progress';
  id: number;
  p: number;
  msg?: string;
}
type WireIn = ResMessage | ErrMessage | ProgressMessage;

/** Main-thread RPC client bound to one dedicated worker. */
export class WorkerRPC {
  private seq = 0;
  private readonly pending = new Map<number, Pending>();
  private readonly onMsg = (e: MessageEvent): void => {
    const data = e.data as WireIn | undefined;
    if (!data || typeof data !== 'object' || typeof data.id !== 'number') return;
    const p = this.pending.get(data.id);
    if (!p) return;
    if (data.type === 'progress') {
      p.onProgress?.(data.p, data.msg);
      return;
    }
    this.pending.delete(data.id);
    if (data.type === 'res') p.resolve(data.result);
    else p.reject(new Error(data.error));
  };

  constructor(private readonly worker: Worker) {
    worker.addEventListener('message', this.onMsg);
  }

  /**
   * Invoke a worker method. `transfer` is the list of `Transferable`s to move
   * into the worker with the request. `onProgress` receives fractional
   * progress (0..1) and an optional status message from the worker.
   */
  call<T>(
    method: string,
    args: unknown,
    transfer?: Transferable[],
    onProgress?: ProgressCb,
  ): Promise<T> {
    const id = ++this.seq;
    // Executor form: resolve/reject are stored and fired later from `onMsg`.
    // (Promise.withResolvers needs ES2024; the project targets ES2022.)
    return new Promise<T>((resolve, reject) => {
      this.pending.set(id, {
        resolve: (v: unknown) => resolve(v as T),
        reject,
        onProgress,
      });
      const msg: ReqMessage = { type: 'req', id, method, args };
      if (transfer && transfer.length > 0) this.worker.postMessage(msg, transfer);
      else this.worker.postMessage(msg);
    });
  }

  terminate(): void {
    this.worker.removeEventListener('message', this.onMsg);
    this.worker.terminate();
    for (const p of this.pending.values()) p.reject(new Error('worker terminated'));
    this.pending.clear();
  }
}

/** Shape returned by a handler when it wants to transfer buffers back. */
export interface TransferResult {
  result: unknown;
  transfer: Transferable[];
}

function isTransferResult(v: unknown): v is TransferResult {
  if (typeof v !== 'object' || v === null) return false;
  if (!('result' in v) || !('transfer' in v)) return false;
  return Array.isArray((v as Record<string, unknown>).transfer);
}

export type RpcHandler = (
  args: unknown,
  onProgress: ProgressCb,
) => Promise<unknown | TransferResult>;

/**
 * Worker-thread server. Register named handlers; each receives the request
 * args and a progress callback that streams fractional progress to the caller.
 */
export function serve(methods: Record<string, RpcHandler>): void {
  const scope = self as unknown as {
    onmessage: ((e: MessageEvent) => void) | null;
    postMessage(message: unknown, transfer?: Transferable[]): void;
  };
  scope.onmessage = async (e: MessageEvent): Promise<void> => {
    const req = e.data as ReqMessage | undefined;
    if (!req || req.type !== 'req' || typeof req.id !== 'number') return;
    const onProgress: ProgressCb = (p, msg) =>
      scope.postMessage({ type: 'progress', id: req.id, p, msg } satisfies ProgressMessage);
    try {
      const fn = methods[req.method];
      if (!fn) throw new Error(`unknown method: ${req.method}`);
      const out = await fn(req.args, onProgress);
      if (isTransferResult(out)) {
        scope.postMessage(
          { type: 'res', id: req.id, result: out.result } satisfies ResMessage,
          out.transfer,
        );
      } else {
        scope.postMessage({ type: 'res', id: req.id, result: out } satisfies ResMessage);
      }
    } catch (err) {
      const error = err instanceof Error ? err.message : String(err);
      scope.postMessage({ type: 'err', id: req.id, error } satisfies ErrMessage);
    }
  };
}
