'use strict';

const OVERFLOW_CODE = 'ERR_CDP_EVENT_OVERFLOW';
const CONNECTION_CLOSED_CODE = 'ERR_CDP_EVENT_CONNECTION_CLOSED';

// CDP payloads come from JSON.parse so a JSON round trip is enough to obtain a
// fully independent deep copy (plain objects and arrays only).
function deepClone(value) {
    if (value === undefined) {
        return undefined;
    }
    return JSON.parse(JSON.stringify(value));
}

function makeOverflowError(capacity, method) {
    const error = new Error(`CDP event observer overflow (capacity ${capacity}) triggered by ${method}`);
    error.code = OVERFLOW_CODE;
    error.capacity = capacity;
    error.method = method;
    return error;
}

function makeConnectionClosedError() {
    const error = new Error('CDP event observer terminated: WebSocket connection closed');
    error.code = CONNECTION_CLOSED_CODE;
    return error;
}

function validateMethods(rawMethods) {
    if (!Array.isArray(rawMethods) || rawMethods.length === 0) {
        throw new TypeError('observe: methods must be a non-empty array of CDP event names');
    }
    const methods = [];
    const seen = new Set();
    for (const method of rawMethods) {
        if (typeof method !== 'string' || method.length === 0) {
            throw new TypeError('observe: event method names must be non-empty strings');
        }
        if (!seen.has(method)) {
            seen.add(method);
            methods.push(method);
        }
    }
    return methods;
}

function validateOptions(options) {
    if (options === null || typeof options !== 'object') {
        throw new TypeError('observe: an options object is required');
    }
    const {capacity, sessionId, signal} = options;
    if (!Number.isInteger(capacity) || capacity <= 0) {
        throw new TypeError('observe: options.capacity must be a positive integer');
    }
    if (!(sessionId === undefined ||
        sessionId === null ||
        (typeof sessionId === 'string' && sessionId.length > 0))) {
        throw new TypeError('observe: options.sessionId must be omitted, null or a non-empty string');
    }
    if (signal !== undefined &&
        (signal === null || typeof signal !== 'object' ||
        typeof signal.addEventListener !== 'function')) {
        throw new TypeError('observe: options.signal must be an AbortSignal');
    }
    return {capacity, sessionId, signal};
}

// create a pull-based async iterator over a subset of the CDP connection
// events, keeping the legacy EventEmitter subscriptions untouched
function createEventObserver(client, rawMethods, rawOptions) {
    // all the validation happens synchronously, before registering anything, so
    // a bad call never leaves a dangling subscription behind
    const methods = validateMethods(rawMethods);
    const {capacity, sessionId, signal} = validateOptions(rawOptions);
    const methodSet = new Set(methods);

    const observer = {
        state: 'active', // active | returned | aborted | overflow | closed
        error: null,
        queue: [],
        waiters: [],
    };

    const matchesSession = (message) => {
        if (sessionId === undefined) {
            return true; // every session, root included
        }
        if (sessionId === null) {
            return message.sessionId === undefined || message.sessionId === null;
        }
        return message.sessionId === sessionId;
    };

    const entryFromMessage = (message) => {
        // settle the iterator copy before the legacy listeners get the message
        const entry = {
            method: message.method,
            params: deepClone(message.params),
        };
        if (message.sessionId !== undefined) {
            entry.sessionId = message.sessionId;
        }
        return entry;
    };

    const detach = () => {
        client._eventObservers.delete(registration);
        if (signal !== undefined) {
            signal.removeEventListener('abort', handleAbort);
        }
    };

    const rejectWaiters = (error) => {
        const waiters = observer.waiters.splice(0);
        for (const waiter of waiters) {
            waiter.reject(error);
        }
    };

    const resolveDoneWaiters = () => {
        const waiters = observer.waiters.splice(0);
        for (const waiter of waiters) {
            waiter.resolve({value: undefined, done: true});
        }
    };

    const handleAbort = () => {
        if (observer.state !== 'active') {
            return;
        }
        // the terminal state is fixed before any consumer callback can run
        observer.state = 'aborted';
        observer.error = signal.reason;
        observer.queue = [];
        detach();
        rejectWaiters(observer.error);
    };

    const dispatch = (message) => {
        if (observer.state !== 'active') {
            return;
        }
        if (!methodSet.has(message.method) || !matchesSession(message)) {
            return;
        }
        // a pending next() gets the event directly, nothing is retained
        const waiter = observer.waiters.shift();
        if (waiter !== undefined) {
            waiter.resolve({value: entryFromMessage(message), done: false});
            return;
        }
        if (observer.queue.length < capacity) {
            observer.queue.push(entryFromMessage(message));
            return;
        }
        // capacity exhausted: terminate, keeping the already queued events so
        // that nothing is silently dropped
        observer.state = 'overflow';
        observer.error = makeOverflowError(capacity, message.method);
        detach();
        rejectWaiters(observer.error);
    };

    const handleConnectionClose = () => {
        if (observer.state !== 'active') {
            return;
        }
        // keep the queue intact so the consumer can drain the events received
        // before the disconnection, only reject once it catches up
        observer.state = 'closed';
        observer.error = makeConnectionClosedError();
        detach();
        rejectWaiters(observer.error);
    };

    const registration = {dispatch, handleConnectionClose};
    client._eventObservers.add(registration);

    // the connection may already be gone (remote drop or client.close()); a
    // fresh observer on a closed WebSocket must terminate instead of hanging
    if (client._ws && client._ws.readyState === 3) {
        handleConnectionClose();
    }

    if (signal !== undefined) {
        if (signal.aborted) {
            handleAbort();
        } else {
            signal.addEventListener('abort', handleAbort, {once: true});
        }
    }

    return {
        [Symbol.asyncIterator]() {
            return this;
        },
        next() {
            // queued events always win, even after the iterator was terminated
            if (observer.queue.length > 0) {
                const value = observer.queue.shift();
                return Promise.resolve({value, done: false});
            }
            if (observer.state === 'active') {
                return new Promise((resolve, reject) => {
                    observer.waiters.push({resolve, reject});
                });
            }
            if (observer.state === 'returned') {
                return Promise.resolve({value: undefined, done: true});
            }
            return Promise.reject(observer.error);
        },
        return() {
            if (observer.state === 'active') {
                observer.state = 'returned';
                observer.queue = [];
                detach();
                resolveDoneWaiters();
            }
            return Promise.resolve({value: undefined, done: true});
        },
        throw(error) {
            if (observer.state === 'active') {
                observer.state = 'returned';
                observer.queue = [];
                detach();
                rejectWaiters(error);
            }
            return Promise.reject(error);
        },
    };
}

module.exports = {
    createEventObserver,
    OVERFLOW_CODE,
    CONNECTION_CLOSED_CODE,
};
