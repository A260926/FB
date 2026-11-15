'use strict';

// Example: drive a form through Runtime.evaluate while collecting the
// Runtime/Network traffic of every target (root page and flattened sessions)
// into structured execution records, one per form action/network request.
//
// Each action is {actionId, sessionId, expression, expectFailure?}. The remote
// endpoint is expected to emit, for every Runtime.evaluate action:
//   - Runtime.consoleAPICalled {args: [{value: actionId}], ...}
//   - Network.requestWillBeSent {requestId, initiator: {actionId}, ...}
//   - Network.loadingFinished or Network.loadingFailed {requestId, ...}
// and only then the command response.
//
// Records are finalized in completion order:
//   {actionId, sessionId, requestId, status, runtimeEvents, failureReason}
// status is one of 'completed', 'failed' or 'incomplete'. An 'incomplete'
// record means the action never reached a network completion because the
// observer overflowed, the page disconnected or the flow was cancelled.
async function runFormAutomation(client, options) {
    const {actions, capacity, signal} = options;

    // the observer never sends any '*.enable' command itself
    const observer = client.observe([
        'Runtime.consoleAPICalled',
        'Network.requestWillBeSent',
        'Network.loadingFinished',
        'Network.loadingFailed',
    ], {capacity, signal});

    const records = [];
    const pendingByAction = new Map();
    const pendingByRequest = new Map();
    const settledByAction = new Map();
    let terminalError = null;

    const finalize = (record, status, extra) => {
        if (record.status !== 'pending') {
            return false;
        }
        record.status = status;
        if (extra) {
            Object.assign(record, extra);
        }
        records.push(record);
        pendingByAction.delete(record.actionId);
        if (record.requestId !== null) {
            pendingByRequest.delete(record.requestId);
        }
        const settle = settledByAction.get(record.actionId);
        if (settle) {
            settledByAction.delete(record.actionId);
            settle();
        }
        return true;
    };

    const markAllIncomplete = (reason) => {
        // copy: finalize() mutates the map while iterating
        for (const record of Array.from(pendingByAction.values())) {
            finalize(record, 'incomplete', {failureReason: reason});
        }
    };

    const incompleteReason = (error) => {
        if (error && error.code === 'ERR_CDP_EVENT_OVERFLOW') {
            return 'overflow';
        }
        if (error && error.code === 'ERR_CDP_EVENT_CONNECTION_CLOSED') {
            return 'disconnected';
        }
        return 'aborted';
    };

    // single consumer merging the intermediate events with the completion one:
    // one request always yields exactly one record, never one per event
    const consume = (async () => {
        try {
            for await (const event of observer) {
                const {method, params} = event;
                const sessionId = event.sessionId === undefined ? null : event.sessionId;
                if (method === 'Runtime.consoleAPICalled') {
                    const actionId = params.args && params.args[0] && params.args[0].value;
                    const record = pendingByAction.get(actionId);
                    if (record) {
                        record.runtimeEvents.push({type: params.type, sessionId});
                    }
                } else if (method === 'Network.requestWillBeSent') {
                    const actionId = params.initiator && params.initiator.actionId;
                    const record = pendingByAction.get(actionId);
                    if (record) {
                        record.requestId = params.requestId;
                        pendingByRequest.set(params.requestId, record);
                    }
                } else if (method === 'Network.loadingFinished') {
                    const record = pendingByRequest.get(params.requestId);
                    if (record) {
                        finalize(record, 'completed');
                    }
                } else if (method === 'Network.loadingFailed') {
                    const record = pendingByRequest.get(params.requestId);
                    if (record) {
                        finalize(record, 'failed', {failureReason: params.errorText});
                    }
                }
            }
        } catch (error) {
            // overflow / disconnection / cancellation: whatever is still in
            // flight is published as incomplete, never as a success
            terminalError = error;
            markAllIncomplete(incompleteReason(error));
            return error;
        }
        return null;
    })();

    try {
        // actions are sent one at a time; the command response is a barrier
        // guaranteeing that all the events of an action have arrived before
        // the next one starts
        for (const action of actions) {
            if (signal && signal.aborted) {
                break; // cancellation: do not start further actions
            }
            const record = {
                actionId: action.actionId,
                sessionId: action.sessionId === undefined ? null : action.sessionId,
                requestId: null,
                status: 'pending',
                runtimeEvents: [],
                failureReason: null,
            };
            pendingByAction.set(action.actionId, record);
            // resolved when the consumer has turned this action's events into a
            // finalized record (or marked it incomplete)
            const settled = new Promise((resolve) => {
                settledByAction.set(action.actionId, resolve);
            });
            try {
                await client.Runtime.evaluate({
                    actionId: action.actionId,
                    expression: action.expression,
                    expectFailure: !!action.expectFailure,
                }, action.sessionId);
                // the observer may have died while the command was in flight;
                // in that case the finally block accounts for this record
                if (terminalError) {
                    break;
                }
                // wait for the consumer, not just for the command response, so
                // no record is still 'pending' when the next action starts
                await settled;
            } catch (error) {
                // the connection died before the command response
                finalize(record, 'incomplete', {failureReason: 'command-failed'});
                break; // a dead connection cannot run any further action
            }
        }
    } finally {
        // let the consumer reach its terminal state first, so the real
        // terminal error (overflow/disconnect/abort) is known
        await observer.return();
        await consume;
        const reason = terminalError ? incompleteReason(terminalError) : 'cancelled';
        // finish whatever is still in flight ...
        markAllIncomplete(reason);
        // ... and account for the actions that were never started, so every
        // input action produces exactly one record
        for (const action of actions) {
            if (!records.some((record) => record.actionId === action.actionId)) {
                records.push({
                    actionId: action.actionId,
                    sessionId: action.sessionId === undefined ? null : action.sessionId,
                    requestId: null,
                    status: 'incomplete',
                    runtimeEvents: [],
                    failureReason: reason,
                });
            }
        }
    }

    return {records, error: terminalError};
}

module.exports = {runFormAutomation};

// direct execution against a real Chrome endpoint is left to the caller, e.g.:
//
// const CDP = require('chrome-remote-interface');
// (async () => {
//     const client = await CDP();
//     await client.Network.enable();
//     const {records} = await runFormAutomation(client, {
//         capacity: 1024,
//         actions: [
//             {actionId: 'fill-email', expression: "document.querySelector('#email').value='a@b.c'"},
//             {actionId: 'submit', expression: "document.querySelector('form').submit()"},
//         ],
//     });
//     console.log(JSON.stringify(records, null, 2));
//     await client.close();
// })();
