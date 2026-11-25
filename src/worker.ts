const fs = require('fs');
import * as path from "path";
import {
    ManageMessageType,
    ManagerMessage,
    ReplayErrorMessage,
    ReplayResultMessage,
    WorkerMessageType,
} from "./protocol";

const crypto = require('crypto');
const {createInstrumenter} = require('istanbul-lib-instrument');
const {hookRequire} = require('istanbul-lib-hook');

let sigint = false;
process.on('SIGINT', function() {
    console.log('Received SIGINT. shutting down gracefully');
    sigint = true;
});

function sha256(text: string): string {
    return crypto.createHash('sha256').update(text, 'utf8').digest('hex');
}

function toBuffer(value: any): Buffer {
    if (Buffer.isBuffer(value)) {
        return Buffer.from(value);
    }
    if (value && value.type === 'Buffer' && Array.isArray(value.data)) {
        return Buffer.from(value.data);
    }
    if (Array.isArray(value)) {
        return Buffer.from(value);
    }
    return Buffer.from(value || []);
}

function asString(value: any): string {
    if (typeof value === 'string') {
        return value;
    }
    return String(value);
}

// The original fuzzing worker: infinite mutation is driven by the manager via
// WORK messages. This path is intentionally unchanged.
class Worker {
    private readonly fn: (buf: Buffer) => void;

    constructor(fn: (buf:Buffer) => void) {
        this.fn = fn;
    }

    getTotalCoverage() {
        let total = 0;
        for (const filePath in global.__coverage__) {
            for (const s in global.__coverage__[filePath].s) {
                total += global.__coverage__[filePath].s[s] ? 1 : 0;
            }
            for (const f in global.__coverage__[filePath].f) {
                total += global.__coverage__[filePath].f[f] ? 1 : 0;
            }
            for (const b in global.__coverage__[filePath].b) {
                for (const i of global.__coverage__[filePath].b[b]) {
                    total += i ? 1 : 0;
                }
            }
        }

        return total
    }

    dump_coverage() {
        const data = JSON.stringify(global.__coverage__);
        if (!fs.existsSync('./.nyc_output')){
            fs.mkdirSync('./.nyc_output');
        }
        fs.writeFileSync('./.nyc_output/cov.json', data);
    }

    start() {
        process.on('message', async (m: ManagerMessage) => {
            try {
                if (m.type === ManageMessageType.WORK) {
                    if (sigint) {
                        this.dump_coverage();
                        process.exit(0);
                    }
                    if (this.fn.constructor.name === 'AsyncFunction') {
                        // @ts-ignore
                        await this.fn(Buffer.from((m.buf as any).data));
                    } else {
                        // @ts-ignore
                        this.fn(Buffer.from((m.buf as any).data));
                    }

                    // @ts-ignore
                    process.send({
                        type: WorkerMessageType.RESULT,
                        coverage: this.getTotalCoverage()
                    })
                }
            } catch (e) {
                console.log("=================================================================");
                console.log(e);
                this.dump_coverage();
                // @ts-ignore
                process.send({
                    type: WorkerMessageType.CRASH,
                });
                process.exit(1);
            }
        });
    }
}

function relativePathWithin(targetDir: string, filePath: string): string | null {
    const rel = path.relative(targetDir, filePath);
    if (rel === '' || path.isAbsolute(rel)) {
        return null;
    }
    if (rel.split(path.sep)[0] === '..') {
        return null;
    }
    return rel.split(path.sep).join('/');
}

function containsNodeModules(filePath: string): boolean {
    return filePath.split(path.sep).indexOf('node_modules') !== -1;
}

// Resolve a path to its on-disk canonical form when possible. This matters on
// platforms where a directory in the path is a symlink (for example /tmp ->
// /private/tmp on macOS): the require hook sees realpaths while the manager
// may have been handed a symlinked path.
function canonicalPath(filePath: string): string {
    try {
        return fs.realpathSync(filePath);
    } catch (e) {
        return path.resolve(filePath);
    }
}

// The replay worker executes normal, well-formed corpus inputs exactly once
// per request and reports the real Istanbul feature set after the target's
// promise has settled. It never invents coverage and never mutates coverage
// counters itself.
class ReplayWorker {
    private readonly targetDir: string;
    private readonly fn: (buf: Buffer) => any;
    private readonly sourceHashes: Map<string, string>;
    private session: string | null;
    private generation: number | null;
    private expectedSeq: number;
    private terminal: boolean;
    private inFlight: string | null;
    private readonly completed: Map<string, ReplayResultMessage>;

    constructor(targetDir: string,
                fn: (buf: Buffer) => any,
                sourceHashes: Map<string, string>) {
        this.targetDir = targetDir;
        this.fn = fn;
        this.sourceHashes = sourceHashes;
        this.session = null;
        this.generation = null;
        this.expectedSeq = 0;
        this.terminal = false;
        this.inFlight = null;
        this.completed = new Map();
    }

    private send(message: any) {
        // @ts-ignore
        if (process.send) {
            // @ts-ignore
            process.send(message);
        }
    }

    private sourceEntries(): Array<[string, string]> {
        const coverage: any = (global as any).__coverage__ || {};
        const entries: Array<[string, string]> = [];
        const seen = new Set<string>();
        for (const absPath of Object.keys(coverage)) {
            const rel = relativePathWithin(this.targetDir, absPath);
            if (rel === null || seen.has(rel)) {
                continue;
            }
            const digest = this.sourceHashes.get(absPath);
            if (!digest) {
                continue;
            }
            seen.add(rel);
            entries.push([rel, digest]);
        }
        entries.sort((a, b) => a[0] < b[0] ? -1 : (a[0] > b[0] ? 1 : 0));
        return entries;
    }

    private features(): string[] {
        const coverage: any = (global as any).__coverage__ || {};
        const features = new Set<string>();
        for (const absPath of Object.keys(coverage)) {
            const rel = relativePathWithin(this.targetDir, absPath);
            if (rel === null) {
                continue;
            }
            const digest = this.sourceHashes.get(absPath);
            if (!digest) {
                continue;
            }
            const fileCoverage = coverage[absPath] || {};
            const statements = fileCoverage.s || {};
            for (const id of Object.keys(statements)) {
                if (statements[id] > 0) {
                    features.add(JSON.stringify([rel, digest, 's', String(id), 0]));
                }
            }
            const functions = fileCoverage.f || {};
            for (const id of Object.keys(functions)) {
                if (functions[id] > 0) {
                    features.add(JSON.stringify([rel, digest, 'f', String(id), 0]));
                }
            }
            const branches = fileCoverage.b || {};
            for (const id of Object.keys(branches)) {
                const counts = branches[id];
                if (!Array.isArray(counts)) {
                    continue;
                }
                for (let index = 0; index < counts.length; index++) {
                    if (counts[index] > 0) {
                        features.add(JSON.stringify([rel, digest, 'b', String(id), index]));
                    }
                }
            }
        }
        const result = Array.from(features);
        result.sort((a, b) => a < b ? -1 : (a > b ? 1 : 0));
        return result;
    }

    announceReady() {
        this.send({
            type: WorkerMessageType.READY,
            sources: this.sourceEntries(),
        });
    }

    private identity(m: ManagerMessage) {
        return {
            session: m.session as string,
            generation: m.generation as number,
            seq: m.seq as number,
            nonce: asString(m.nonce),
        };
    }

    private sendError(m: ManagerMessage, message: string) {
        const error: ReplayErrorMessage = {
            type: WorkerMessageType.REPLAY_ERROR,
            session: m.session,
            generation: m.generation,
            seq: m.seq,
            nonce: m.nonce,
            message,
        };
        this.send(error);
    }

    start() {
        process.on('message', async (m: ManagerMessage) => {
            if (!m || typeof m !== 'object') {
                return;
            }
            if (m.type === ManageMessageType.STOP) {
                process.exit(0);
                return;
            }
            if (m.type !== ManageMessageType.REPLAY) {
                return;
            }

            if (typeof m.session !== 'string' || m.session.length === 0 ||
                typeof m.nonce !== 'string' || m.nonce.length === 0 ||
                typeof m.generation !== 'number' ||
                !Number.isInteger(m.generation) ||
                typeof m.seq !== 'number' ||
                !Number.isInteger(m.seq) || m.seq < 0) {
                this.sendError(m, 'malformed replay request');
                return;
            }

            if (this.session !== null && m.session !== this.session) {
                // Not ours; do not touch target state.
                this.sendError(m, 'replay request belongs to another session');
                return;
            }
            if (this.generation !== null && m.generation !== this.generation) {
                this.sendError(m, 'replay request belongs to another worker generation');
                return;
            }

            const key = `${m.session}|${m.generation}|${m.seq}`;
            const cached = this.completed.get(key);
            if (cached) {
                // A duplicate delivery of an executed request is answered from
                // the cached result, so target memory cannot advance twice and
                // coverage cannot be inserted twice.
                this.send(cached);
                return;
            }

            if (this.terminal) {
                this.sendError(m, 'replay session is terminated');
                return;
            }
            if (this.inFlight !== null) {
                // Another execution is still outstanding. Accepting this one
                // would interleave target calls; refuse it instead.
                this.sendError(m, 'replay worker is busy with a previous request');
                return;
            }
            if (m.seq !== this.expectedSeq) {
                // Out-of-order delivery is refused before execution rather than
                // letting the target observe inputs in the wrong order.
                this.sendError(m,
                    `replay request out of order (expected ${this.expectedSeq})`);
                return;
            }

            if (this.session === null) {
                this.session = m.session;
                this.generation = m.generation;
            }

            this.inFlight = key;
            try {
                const input = toBuffer(m.buf);
                const result: any = this.fn(input);
                if (result && typeof result.then === 'function') {
                    await result;
                }
                const features = this.features();
                const message: ReplayResultMessage = {
                    type: WorkerMessageType.REPLAY_RESULT,
                    ...this.identity(m),
                    features,
                    sources: this.sourceEntries(),
                    coverage: features.length,
                };
                this.completed.set(key, message);
                this.expectedSeq = this.expectedSeq + 1;
                this.inFlight = null;
                this.send(message);
            } catch (e) {
                this.inFlight = null;
                this.terminal = true;
                this.sendError(m, String((e && e.stack) || e));
            }
        });
    }
}

const replayMode = process.argv[3] === '--replay';
// @ts-ignore
const fuzzTargetPath = path.resolve(process.cwd(), process.argv[2]);
const fuzzTargetRealPath = canonicalPath(fuzzTargetPath);

if (!replayMode) {
    const instrumenter = createInstrumenter({compact: true});
    // @ts-ignore
    hookRequire((filePath) => true, (code, {filename}) => {
        const newCode = instrumenter.instrumentSync(code, filename);
        return newCode;
    });

    // @ts-ignore
    const fuzzTargetFn = require(fuzzTargetPath).fuzz;
    const worker = new Worker(fuzzTargetFn);
    worker.start();
} else {
    const targetDir = path.dirname(fuzzTargetRealPath);
    const instrumenter = createInstrumenter({compact: true});
    const sourceHashes = new Map<string, string>();
    // @ts-ignore
    hookRequire((filePath: string) => {
        // Node's require machinery normalizes to an absolute realpath; compare
        // on the same canonical form as the target directory.
        const resolved = canonicalPath(filePath);
        if (relativePathWithin(targetDir, resolved) !== null) {
            return true;
        }
        if (containsNodeModules(resolved)) {
            // Dependency packages are not target modules and are deliberately
            // not instrumented or counted.
            return false;
        }
        // Any other module escaping the target directory would produce
        // coverage whose relative feature path could not be represented;
        // refuse rather than silently instrument it.
        throw new Error(
            `replay refuses to instrument module outside target directory: ${resolved}`);
    }, (code: string, opts: {filename: string}) => {
        const resolved = canonicalPath(opts.filename);
        sourceHashes.set(resolved, sha256(code));
        return instrumenter.instrumentSync(code, resolved);
    });

    let fuzzTargetFn: (buf: Buffer) => any = () => {
        throw new Error('replay target was not initialized');
    };
    try {
        const targetModule = require(fuzzTargetRealPath);
        if (!targetModule || typeof targetModule.fuzz !== 'function') {
            throw new Error('replay target does not export a fuzz function');
        }
        fuzzTargetFn = targetModule.fuzz;
    } catch (e) {
        console.error(String((e && (e as any).stack) || e));
        // @ts-ignore
        if (process.send) {
            // @ts-ignore
            process.send({
                type: WorkerMessageType.REPLAY_ERROR,
                message: String((e && (e as any).message) || e),
            });
        }
        process.exit(1);
    }

    const replayWorker = new ReplayWorker(targetDir, fuzzTargetFn, sourceHashes);
    replayWorker.announceReady();
    replayWorker.start();
}
