import type { CrawlerResponse, RequestOptions } from "./types/crawler.js";

type OrderedEntry = {
    seq: number;
    options: RequestOptions;
    limiterRelease: () => void;
    ready: boolean;
    dispatched: boolean;
    released: boolean;
    error: unknown;
    response: CrawlerResponse;
};

const isThenable = (value: unknown): value is PromiseLike<unknown> =>
    value !== null && (typeof value === "object" || typeof value === "function") &&
    typeof (value as PromiseLike<unknown>).then === "function";

/**
 * Serializes callback delivery for crawler requests without serializing the
 * requests themselves. Every task actually started by a rate limiter gets a
 * sequence number on start; its callback is only invoked when every earlier
 * entry has already been delivered, and the limiter slot stays occupied until
 * the user calls `done` (or the entry is released automatically).
 */
export class OrderedCallbackQueue {
    private _entries = new Map<number, OrderedEntry>();
    private _optionsIndex = new WeakMap<RequestOptions, OrderedEntry>();
    private _nextSeq = 0;
    private _deliverSeq = 0;
    private _delivering = false;
    private _onCallbackError: (error: unknown, response: CrawlerResponse) => void;

    constructor(handlers: { onCallbackError: (error: unknown, response: CrawlerResponse) => void }) {
        this._onCallbackError = handlers.onCallbackError;
    }

    has(options: RequestOptions): boolean {
        return this._optionsIndex.has(options);
    }

    /**
     * Reserve a delivery position. Must be called when the rate limiter
     * actually starts the task, not when the request is added or scheduled.
     */
    register(options: RequestOptions, limiterRelease: () => void): void {
        const existing = this._optionsIndex.get(options);
        if (existing) return;
        const entry: OrderedEntry = {
            seq: this._nextSeq++,
            options,
            limiterRelease,
            ready: false,
            dispatched: false,
            released: false,
            error: null,
            response: void 0,
        };
        this._entries.set(entry.seq, entry);
        this._optionsIndex.set(options, entry);
    }

    /**
     * Mark a request as finished. Mid-flight retries do not call this; only
     * a success or the final failure after retries are exhausted is ready.
     */
    markReady(options: RequestOptions, error: unknown, response: CrawlerResponse): void {
        const entry = this._optionsIndex.get(options);
        if (!entry || entry.ready) return;
        entry.ready = true;
        entry.error = error;
        entry.response = response;
        this._pump();
    }

    /**
     * Release the limiter slot held by an entry. Idempotent: repeated calls
     * (including a `done` followed by a thrown error) only ever release the
     * entry's own slot once.
     */
    release(options: RequestOptions): boolean {
        const entry = this._optionsIndex.get(options);
        if (!entry) return false;
        return this._release(entry);
    }

    private _release(entry: OrderedEntry): boolean {
        if (entry.released) return false;
        entry.released = true;
        entry.limiterRelease();
        this._pump();
        return true;
    }

    private _pump(): void {
        // Re-entrant calls (done() inside a callback, add() inside a callback)
        // must not nest callback invocations; the outer loop keeps draining
        // once the current callback body has returned.
        if (this._delivering) return;
        this._delivering = true;
        try {
            for (;;) {
                const entry = this._entries.get(this._deliverSeq);
                if (!entry || !entry.ready) break;
                if (entry.dispatched) {
                    // The callback body has run. The head can only move once
                    // its own done has released the limiter slot.
                    if (!entry.released) break;
                    this._entries.delete(entry.seq);
                    this._deliverSeq++;
                    continue;
                }
                entry.dispatched = true;
                this._dispatch(entry);
                if (!entry.released) break;
            }
        } finally {
            this._delivering = false;
        }
    }

    private _dispatch(entry: OrderedEntry): void {
        const callback = entry.options.callback;
        if (typeof callback !== "function") {
            // Mirror the default behavior: without a user callback the task
            // releases its slot as soon as it is delivered.
            this._release(entry);
            return;
        }

        const done = (): void => {
            this._release(entry);
        };

        try {
            const result = callback(entry.error, entry.response, done);
            if (isThenable(result)) {
                // A resolved promise is NOT an implicit done. A rejected one
                // ends the entry and is reported via callbackError.
                Promise.resolve(result).then(
                    () => {},
                    (error: unknown) => {
                        this._release(entry);
                        this._onCallbackError(error, entry.response);
                    }
                );
            }
        } catch (error) {
            // Idempotent: if the callback already called done, this only
            // reports the error and never releases the slot twice.
            this._release(entry);
            this._onCallbackError(error, entry.response);
        }
    }
}

export default OrderedCallbackQueue;
