'use strict';

// In-process WebSocket transport used by the tests: the sandbox forbids
// binding real TCP/Unix sockets, so a pair of Duplex streams replaces the
// network while the real 'ws' framing (Receiver/Sender) is still exercised.
const {Duplex} = require('node:stream');
const WebSocket = require('ws');

class MemSocket extends Duplex {
    constructor() {
        super();
        this.peer = null;
        this._pending = null;
        this._flushing = false;
    }

    _write(chunk, encoding, callback) {
        // 'ws' needs the write callback (which registers command replies) to
        // run before the peer observes the frame
        callback();
        // coalesce every frame written in the same turn into one chunk, like a
        // real TCP socket delivering a burst of packets together
        if (this._pending === null) {
            this._pending = Buffer.from(chunk);
        } else {
            this._pending = Buffer.concat([this._pending, chunk]);
        }
        if (!this._flushing) {
            this._flushing = true;
            setImmediate(() => this._flush());
        }
    }

    _flush() {
        const chunk = this._pending;
        this._pending = null;
        this._flushing = false;
        if (this.peer && !this.peer.destroyed) {
            this.peer.push(chunk);
        }
    }

    _read() {
        // pushed data is delivered automatically
    }

    _final(callback) {
        // emulate TCP half-close: ending the writable side signals EOF to the
        // readable side of the peer
        setImmediate(() => {
            if (this.peer && !this.peer.destroyed) {
                this.peer.push(null);
            }
            callback();
        });
    }

    setTimeout() {
        // no network timers
    }

    setNoDelay() {
        // no network buffering
    }

    destroy(error, callback) {
        const peer = this.peer;
        super.destroy(error, callback);
        // emulate the network: one end going away closes the other end too
        if (peer && !peer.destroyed) {
            peer.destroy();
        }
    }
}

function createSocketPair() {
    const clientSocket = new MemSocket();
    const serverSocket = new MemSocket();
    clientSocket.peer = serverSocket;
    serverSocket.peer = clientSocket;
    return [clientSocket, serverSocket];
}

function createWebSocketPair() {
    const [clientSocket, serverSocket] = createSocketPair();
    // frames sent by the client must be masked, server frames must not
    const client = new WebSocket(null);
    client._isServer = false;
    const server = new WebSocket(null);
    // a real handshake completes asynchronously, so the 'open' event must be
    // emitted after the caller attached its listeners
    process.nextTick(() => {
        client.setSocket(clientSocket, Buffer.alloc(0), 256 * 1024 * 1024);
        server.setSocket(serverSocket, Buffer.alloc(0), 256 * 1024 * 1024);
    });
    return {client, server, clientSocket, serverSocket};
}

// minimal stand-in for a WebSocket.Server: every factory call opens a new
// in-process connection and hands the server-side WebSocket to 'handler'
class MemWSHub {
    constructor(handler) {
        this.handler = handler;
        this.servers = [];
    }

    createWebSocket() {
        const {client, server} = createWebSocketPair();
        this.servers.push(server);
        this.handler(server);
        return client;
    }

    // simulate the remote endpoint dropping the connection without a
    // WebSocket closing handshake
    disconnect(server) {
        const target = server || this.servers[this.servers.length - 1];
        target._socket.destroy();
    }

    close() {
        for (const server of this.servers) {
            if (server.readyState !== WebSocket.CLOSED) {
                server._socket.destroy();
            }
        }
        this.servers = [];
    }
}

module.exports = {
    MemSocket,
    createSocketPair,
    createWebSocketPair,
    MemWSHub,
};
