import * as crypto from "crypto";
import * as path from "path";
import {fork, ChildProcess} from "child_process";
import {Corpus} from "./corpus";
import {
    ManageMessageType,
    ManagerMessage,
    ReplayErrorMessage,
    ReplayResultMessage,
    WorkerMessageType,
} from "./protocol";

// ---------------------------------------------------------------------------
// Checkpoint format
// ---------------------------------------------------------------------------
//
// A checkpoint is a self-contained buffer. It embeds the complete execution
// plan (every stage with its inputs), the next stage/cursor, the cumulative
// feature set after every completed item and the corpus produced so far. It
// contains no PIDs and no absolute paths so that identical sessions serialize
// identically.

const CHECKPOINT_MAGIC = 'jsfuzz-replay-checkpoint';
const CHECKPOINT_VERSION = 1;

interface CheckpointItem {
    stage: number;
    cursor: number;
    features: string[];
    sources: Array<[string, string]>;
    added: boolean;
    input?: Buffer;
}

interface CheckpointData {
    version: number;
    buildID: string;
    stages: Buffer[][];
    stage: number;
    cursor: number;
    executions: number;
    features: string[];
    sources: Array<[string, string]>;
    corpus: Buffer[];
    items: CheckpointItem[];
}

interface StateSnapshot {
    stage: number;
    cursor: number;
    executions: number;
    corpusSize: number;
    coverage: string[];
}

function sha256(data: Buffer | string): string {
    return crypto.createHash('sha256').update(data).digest('hex');
}

function canonicalJson(value: any): string {
    if (value === null || typeof value !== 'object') {
        return JSON.stringify(value);
    }
    if (Array.isArray(value)) {
        return '[' + value.map(canonicalJson).join(',') + ']';
    }
    const keys = Object.keys(value).sort();
    let out = '{';
    for (let i = 0; i < keys.length; i++) {
        if (i !== 0) {
            out += ',';
        }
        out += JSON.stringify(keys[i]) + ':' + canonicalJson(value[keys[i]]);
    }
    return out + '}';
}

function decodeBase64(text: string, what: string): Buffer {
    if (typeof text !== 'string' || !/^[A-Za-z0-9+/]*={0,2}$/.test(text) ||
        text.length % 4 !== 0) {
        throw new Error(`checkpoint ${what} is not valid base64`);
    }
    const buf = Buffer.from(text, 'base64');
    if (buf.toString('base64') !== text) {
        throw new Error(`checkpoint ${what} is not valid base64`);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Worker protocol helpers
// ---------------------------------------------------------------------------

interface WorkerReply {
    message: any;
}

class WorkerHandle {
    readonly generation: number;
    private readonly child: ChildProcess;
    private readonly pending: Map<string, (reply: WorkerReply) => void>;
    private readonly failures: Array<(err: Error) => void>;
    private requestSeq: number;
    private closed: boolean;
    private exited: boolean;
    private terminalError: Error | null;
    private terminating: Promise<void> | null;
    private readonly readyPromise: Promise<Array<[string, string]>>;
    private readonly sourceHashes: Map<string, string>;
    private preReadyError: string | null;

    constructor(child: ChildProcess, generation: number) {
        this.child = child;
        this.generation = generation;
        this.pending = new Map();
        this.failures = [];
        this.requestSeq = 0;
        this.closed = false;
        this.exited = false;
        this.terminalError = null;
        this.terminating = null;
        this.sourceHashes = new Map();
        this.preReadyError = null;

        let resolveReady: (sources: Array<[string, string]>) => void;
        let rejectReady: (err: Error) => void;
        this.readyPromise = new Promise<Array<[string, string]>>((resolve, reject) => {
            resolveReady = resolve;
            rejectReady = reject;
        });

        let readySettled = false;
        child.on('message', (message: any) => {
            if (!message || typeof message !== 'object') {
                return;
            }
            if (message.type === WorkerMessageType.READY && !readySettled) {
                readySettled = true;
                const sources = (message.sources || []) as Array<[string, string]>;
                for (const entry of sources) {
                    this.sourceHashes.set(entry[0], entry[1]);
                }
                resolveReady(sources);
                return;
            }
            if (message.type === WorkerMessageType.REPLAY_ERROR && !readySettled &&
                typeof message.message === 'string' && message.session === undefined) {
                // A target-load failure arrives before READY; remember the
                // real reason so the exit error is not the only signal.
                this.preReadyError = message.message;
                return;
            }
            if (message.type === WorkerMessageType.REPLAY_RESULT ||
                message.type === WorkerMessageType.REPLAY_ERROR) {
                const key = this.key(message.session, message.generation,
                    message.seq, message.nonce);
                const handler = this.pending.get(key);
                if (handler) {
                    this.pending.delete(key);
                    handler({message});
                }
                return;
            }
            // RESULT/CRASH belong to the fuzzing worker; nothing to do here.
        });

        child.on('error', (err: Error) => {
            this.failAll(err);
        });
        child.on('exit', (code: number | null, signal: string | null) => {
            this.exited = true;
            if (!readySettled) {
                readySettled = true;
                const reason = this.preReadyError ?
                    `: ${this.preReadyError}` : '';
                rejectReady(new Error(
                    `replay worker exited before ready (code=${code}, signal=${signal})${reason}`));
            }
            this.failAll(new Error(
                `replay worker exited (code=${code}, signal=${signal})`));
        });
    }

    private key(session: string, generation: number, seq: number,
                nonce: string): string {
        return `${session}|${generation}|${seq}|${nonce}`;
    }

    private failAll(err: Error) {
        this.terminalError = err;
        for (const handler of Array.from(this.pending.values())) {
            handler({message: null});
        }
        this.pending.clear();
        for (const handler of Array.from(this.failures)) {
            handler(err);
        }
        this.failures.length = 0;
    }

    whenReady(): Promise<Array<[string, string]>> {
        return this.readyPromise;
    }

    onFailure(handler: (err: Error) => void) {
        if (this.terminalError) {
            handler(this.terminalError);
            return;
        }
        this.failures.push(handler);
    }

    get pid(): number | undefined {
        return this.child.pid;
    }

    isAlive(): boolean {
        return !this.exited && !this.closed;
    }

    sourceHash(rel: string): string | undefined {
        return this.sourceHashes.get(rel);
    }

    async sendReplay(session: string, seq: number, nonce: string, buf: Buffer,
                     generation: number, timeoutMs: number):
        Promise<ReplayResultMessage | ReplayErrorMessage> {
        if (this.closed || this.exited) {
            throw new Error('replay worker is not running');
        }
        if (this.terminalError) {
            throw this.terminalError;
        }
        const key = this.key(session, generation, seq, nonce);
        return new Promise<ReplayResultMessage | ReplayErrorMessage>((resolve, reject) => {
            let timer: NodeJS.Timeout | null = null;
            const finish = (fn: () => void) => {
                if (timer) {
                    clearTimeout(timer);
                    timer = null;
                }
                fn();
            };
            this.pending.set(key, (reply: WorkerReply) => {
                if (!reply.message) {
                    finish(() => reject(
                        this.terminalError || new Error('replay worker terminated')));
                    return;
                }
                finish(() => resolve(
                    reply.message as ReplayResultMessage | ReplayErrorMessage));
            });
            timer = setTimeout(() => {
                if (this.pending.delete(key)) {
                    reject(new Error('replay worker did not answer in time'));
                }
            }, timeoutMs);
            if (!this.child.connected) {
                this.pending.delete(key);
                finish(() => reject(new Error('replay worker IPC channel is closed')));
                return;
            }
            const message: ManagerMessage = {
                type: ManageMessageType.REPLAY,
                buf: buf,
                session,
                generation,
                seq,
                nonce,
            };
            this.child.send(message as any, (err: Error | null) => {
                if (err) {
                    if (this.pending.delete(key)) {
                        finish(() => reject(err));
                    }
                }
            });
        });
    }

    nextSeq(): number {
        this.requestSeq += 1;
        return this.requestSeq - 1;
    }

    terminate(): Promise<void> {
        if (this.terminating) {
            return this.terminating;
        }
        this.terminating = this.performTermination();
        return this.terminating;
    }

    private performTermination(): Promise<void> {
        this.closed = true;
        if (this.exited) {
            return Promise.resolve();
        }
        return new Promise<void>((resolve) => {
            const child = this.child;
            let settled = false;
            const done = () => {
                if (settled) {
                    return;
                }
                settled = true;
                resolve();
            };
            child.once('exit', done);
            try {
                if (child.connected) {
                    child.send({type: ManageMessageType.STOP}, () => {
                        // Ignore send errors; the kill timers below enforce exit.
                    });
                }
            } catch (e) {
                // fall through to kill
            }
            // Give STOP a moment to take effect, then force the process down.
            const hardStop = setTimeout(() => {
                try {
                    child.kill('SIGKILL');
                } catch (e) {
                    // already gone
                }
            }, 500);
            if (typeof (hardStop as any).unref === 'function') {
                (hardStop as any).unref();
            }
            child.once('exit', () => {
                clearTimeout(hardStop);
                done();
            });
        });
    }
}

// ---------------------------------------------------------------------------
// Replay controller
// ---------------------------------------------------------------------------

interface PendingStep {
    seq: number;
    nonce: string;
    stage: number;
    cursor: number;
    input: Buffer;
    expectedSources: Array<[string, string]>;
    cumulativeFeatures: string[];
    added: boolean;
}

export class ReplayController {
    private readonly buildID: string;
    private readonly stages: Buffer[][];
    private readonly session: string;
    private worker: WorkerHandle;
    private generation: number;
    private stage: number;
    private cursor: number;
    private executions: number;
    private features: string[];
    private sources: Array<[string, string]>;
    private corpus: Corpus;
    private corpusDigests: Set<string>;
    private items: CheckpointItem[];
    private busy: boolean;
    private terminated: Error | null;
    private closed: boolean;

    private constructor(buildID: string,
                        stages: Buffer[][],
                        worker: WorkerHandle,
                        generation: number) {
        this.buildID = buildID;
        this.stages = stages;
        this.worker = worker;
        this.generation = generation;
        this.stage = 0;
        this.cursor = 0;
        this.executions = 0;
        this.features = [];
        this.sources = [];
        this.corpus = new Corpus([], false);
        this.corpusDigests = new Set();
        this.items = [];
        this.busy = false;
        this.terminated = null;
        this.closed = false;
        this.session = crypto.randomBytes(16).toString('hex');
        const handle = this.worker;
        handle.onFailure((err) => this.fail(err));
    }

    // -- lifecycle --------------------------------------------------------

    static async create(target: string, stages: Buffer[][], buildID: string):
        Promise<ReplayController> {
        ReplayController.validateArguments(target, stages, buildID);
        const normalized = ReplayController.normalizeStages(stages);
        const worker = ReplayController.spawnWorker(target);
        try {
            await worker.whenReady();
            const controller = new ReplayController(
                buildID, normalized, worker, 1);
            await controller.finishConstruction();
            return controller;
        } catch (e) {
            await worker.terminate();
            throw e;
        }
    }

    static async restore(target: string, checkpoint: Buffer, buildID: string):
        Promise<ReplayController> {
        if (typeof buildID !== 'string' || buildID.length === 0) {
            throw new Error('buildID must be a non-empty string');
        }
        if (!Buffer.isBuffer(checkpoint)) {
            throw new Error('checkpoint must be a Buffer');
        }
        // Validate everything about the checkpoint before any worker exists.
        const data = ReplayController.decodeCheckpoint(checkpoint, buildID);
        ReplayController.validateArguments(target, data.stages, buildID);
        const plan = ReplayController.normalizeStages(data.stages);
        const worker = ReplayController.spawnWorker(target);
        try {
            await worker.whenReady();
            const controller = new ReplayController(
                buildID, plan, worker, 1);
            try {
                await controller.replayPrefix(data);
            } catch (e) {
                await worker.terminate();
                throw e;
            }
            return controller;
        } catch (e) {
            await worker.terminate();
            throw e;
        }
    }

    private static validateArguments(target: string, stages: Buffer[][],
                                     buildID: string) {
        if (typeof target !== 'string' || target.length === 0) {
            throw new Error('target must be a non-empty path string');
        }
        if (typeof buildID !== 'string' || buildID.length === 0) {
            throw new Error('buildID must be a non-empty string');
        }
        if (!Array.isArray(stages)) {
            throw new Error('stages must be an array');
        }
        for (const stage of stages) {
            if (!Array.isArray(stage)) {
                throw new Error('each stage must be an array of Buffers');
            }
            for (const input of stage) {
                if (!Buffer.isBuffer(input)) {
                    throw new Error('each stage input must be a Buffer');
                }
            }
        }
    }

    private static normalizeStages(stages: Buffer[][]): Buffer[][] {
        return stages.map(stage => stage.map(input => Buffer.from(input)));
    }

    private static planDigestFromBuffers(stages: Buffer[][]): string {
        return sha256(canonicalJson(
            stages.map(stage => stage.map(input => input.toString('hex')))));
    }

    private static spawnWorker(target: string): WorkerHandle {
        const resolvedTarget = path.resolve(target);
        let child: ChildProcess;
        try {
            child = fork(path.join(__dirname, 'worker.js'),
                [resolvedTarget, '--replay'],
                {execArgv: [], stdio: ['ignore', 'pipe', 'pipe', 'ipc']});
        } catch (e) {
            throw e;
        }
        // Keep the worker's stdout/stderr from blocking; capture for diagnostics
        // only (never counted as protocol data).
        if (child.stdout) {
            child.stdout.on('data', () => {});
        }
        if (child.stderr) {
            child.stderr.on('data', () => {});
        }
        return new WorkerHandle(child, 1);
    }

    private async finishConstruction() {
        const sources = await this.worker.whenReady();
        this.sources = ReplayController.sortedSources(sources);
        this.features = [];
    }

    // -- state / checkpoint ----------------------------------------------

    state(): StateSnapshot {
        // Read-only: report the last committed snapshot even after the session
        // has terminated or been closed. Only work-producing calls (step and
        // checkpoint) are refused.
        const target = this.nextPointer();
        return {
            stage: target.stage,
            cursor: target.cursor,
            executions: this.executions,
            corpusSize: this.corpus.getLength(),
            coverage: this.features.slice(),
        };
    }

    async checkpoint(): Promise<Buffer> {
        if (this.closed) {
            throw new Error('replay controller is closed');
        }
        if (this.busy) {
            throw new Error('replay controller is busy');
        }
        this.assertUsable();
        if (this.worker.isAlive()) {
            const sources = await this.worker.whenReady();
            if (!ReplayController.sourcesEqual(this.sources, sources)) {
                throw new Error('target sources changed during the replay session');
            }
        }
        const data: CheckpointData = {
            version: CHECKPOINT_VERSION,
            buildID: this.buildID,
            stages: this.stages.map(stage => stage.map(input => Buffer.from(input))),
            stage: this.nextPointer().stage,
            cursor: this.nextPointer().cursor,
            executions: this.executions,
            features: this.features.slice(),
            sources: this.sources.map(entry => [entry[0], entry[1]]),
            corpus: this.corpus.getBuffers().map(buf => Buffer.from(buf)),
            items: this.items.map(item => ({
                stage: item.stage,
                cursor: item.cursor,
                features: item.features.slice(),
                sources: item.sources.map(entry => [entry[0], entry[1]]),
                added: item.added,
            })),
        };
        return ReplayController.encodeCheckpoint(data);
    }

    async close(): Promise<void> {
        if (this.closed) {
            return;
        }
        this.closed = true;
        await this.worker.terminate();
    }

    // -- stepping ---------------------------------------------------------

    async step(): Promise<boolean> {
        if (this.closed) {
            throw new Error('replay controller is closed');
        }
        if (this.busy) {
            throw new Error('replay controller is busy');
        }
        if (this.terminated) {
            throw this.terminated;
        }
        const pointer = this.nextPointer();
        if (pointer.done) {
            return false;
        }
        this.busy = true;
        try {
            const seq = this.worker.nextSeq();
            const nonce = crypto.randomBytes(16).toString('hex');
            const input = Buffer.from(this.stages[pointer.stage][pointer.cursor]);
            const message = await this.worker.sendReplay(
                this.session, seq, nonce, input, this.generation,
                this.timeoutMs());
            if (!ReplayController.matchesIdentity(message, this.session,
                this.generation, seq, nonce)) {
                throw new Error('replay worker answered a different request');
            }
            if (message.type !== WorkerMessageType.REPLAY_RESULT) {
                throw new Error(`replay execution failed: ${message.message}`);
            }
            const observedSources = ReplayController.sortedSources(
                message.sources || []);
            if (!ReplayController.sourcesEqual(this.sources, observedSources)) {
                throw new Error('target source set changed during replay');
            }
            const observed = ReplayController.validateFeatureList(
                message.features, this.sources);
            const cumulative = ReplayController.unionSorted(this.features, observed);
            const added = cumulative.length !== this.features.length;
            this.features = cumulative;
            this.executions += 1;
            this.items.push({
                stage: pointer.stage,
                cursor: pointer.cursor,
                features: cumulative.slice(),
                sources: this.sources.map(entry => [entry[0], entry[1]]),
                added,
            });
            if (added) {
                const digest = sha256(input);
                if (!this.corpusDigests.has(digest)) {
                    this.corpus.putBuffer(input);
                    this.corpusDigests.add(digest);
                }
            }
            this.advancePointer();
            return true;
        } catch (e) {
            this.fail(e);
            throw e;
        } finally {
            this.busy = false;
        }
    }

    // -- internals -------------------------------------------------------

    private timeoutMs(): number {
        // The regular CLI defaults to a 30s per-testcase timeout. Replay is an
        // explicit, bounded driver, so the same default keeps behaviour
        // predictable for slow-but-valid targets.
        return 30000;
    }

    private nextPointer(): {stage: number, cursor: number, done: boolean} {
        let stage = this.stage;
        let cursor = this.cursor;
        while (stage < this.stages.length &&
               cursor >= this.stages[stage].length) {
            stage += 1;
            cursor = 0;
        }
        if (stage >= this.stages.length) {
            return {stage: this.stages.length, cursor: 0, done: true};
        }
        return {stage, cursor, done: false};
    }

    private advancePointer() {
        let stage = this.stage;
        let cursor = this.cursor + 1;
        while (stage < this.stages.length &&
               cursor >= this.stages[stage].length) {
            stage += 1;
            cursor = 0;
        }
        if (stage >= this.stages.length) {
            this.stage = this.stages.length;
            this.cursor = 0;
        } else {
            this.stage = stage;
            this.cursor = cursor;
        }
    }

    private fail(err: any) {
        if (!this.terminated) {
            this.terminated = err instanceof Error ? err : new Error(String(err));
        }
        // An execution/IPC failure leaves the session unusable; reclaim the
        // process so no orphan remains.
        this.worker.terminate();
    }

    private assertUsable() {
        if (this.terminated) {
            throw this.terminated;
        }
    }

    private static matchesIdentity(message: any, session: string,
                                   generation: number, seq: number,
                                   nonce: string): boolean {
        return message && message.session === session &&
            message.generation === generation &&
            message.seq === seq &&
            message.nonce === nonce;
    }

    private static sortedSources(sources: Array<[string, string]>):
        Array<[string, string]> {
        const copy = sources.map(entry => [String(entry[0]), String(entry[1])] as [string, string]);
        copy.sort((a, b) => a[0] < b[0] ? -1 : (a[0] > b[0] ? 1 : 0));
        return copy;
    }

    private static sourcesEqual(a: Array<[string, string]>,
                                b: Array<[string, string]>): boolean {
        return canonicalJson(a) === canonicalJson(b);
    }

    private static validateFeatureList(features: any,
                                       sources: Array<[string, string]>):
        string[] {
        if (!Array.isArray(features)) {
            throw new Error('replay worker returned a non-array feature set');
        }
        const digests = new Map<string, string>();
        for (const entry of sources) {
            digests.set(entry[0], entry[1]);
        }
        const result: string[] = [];
        let previous: string | null = null;
        for (const raw of features) {
            if (typeof raw !== 'string') {
                throw new Error('replay worker returned a non-string feature');
            }
            let feature: any;
            try {
                feature = JSON.parse(raw);
            } catch (e) {
                throw new Error('replay worker returned a malformed feature');
            }
            if (!Array.isArray(feature) || feature.length !== 5) {
                throw new Error('replay worker returned a malformed feature');
            }
            const [rel, digest, kind, id, index] = feature;
            if (typeof rel !== 'string' || typeof digest !== 'string' ||
                (kind !== 's' && kind !== 'f' && kind !== 'b') ||
                typeof id !== 'string' || !/^[0-9]+$/.test(id) ||
                typeof index !== 'number' ||
                !Number.isInteger(index) || index < 0) {
                throw new Error('replay worker returned a malformed feature');
            }
            const expectedDigest = digests.get(rel);
            if (expectedDigest === undefined || expectedDigest !== digest) {
                throw new Error(
                    `replay feature binds unknown source ${rel}`);
            }
            if ((kind !== 'b' && index !== 0)) {
                throw new Error('replay statement/function feature has non-zero index');
            }
            if (previous !== null && !(previous < raw)) {
                throw new Error('replay worker returned an unsorted feature set');
            }
            previous = raw;
            result.push(raw);
        }
        return result;
    }

    private static unionSorted(a: string[], b: string[]): string[] {
        const union = new Set<string>(a);
        for (const value of b) {
            union.add(value);
        }
        return Array.from(union).sort((x, y) => x < y ? -1 : (x > y ? 1 : 0));
    }

    // -- checkpoint encoding / decoding ----------------------------------

    private static encodeCheckpoint(data: CheckpointData): Buffer {
        const body = {
            version: data.version,
            buildID: data.buildID,
            planDigest: ReplayController.planDigestFromBuffers(data.stages),
            stages: data.stages.map(stage => stage.map(input => input.toString('base64'))),
            stage: data.stage,
            cursor: data.cursor,
            executions: data.executions,
            features: data.features,
            sources: data.sources,
            corpus: data.corpus.map(buf => buf.toString('base64')),
            items: data.items.map(item => ({
                stage: item.stage,
                cursor: item.cursor,
                features: item.features,
                sources: item.sources,
                added: item.added,
            })),
        };
        const canonical = canonicalJson(body);
        const digest = sha256(canonical);
        return Buffer.from(
            `${CHECKPOINT_MAGIC}\n${CHECKPOINT_VERSION}\n${digest}\n${canonical}\n`,
            'utf8');
    }

    private static decodeCheckpoint(blob: Buffer, buildID: string): CheckpointData {
        const text = blob.toString('utf8');
        if (blob.length === 0 || text.length === 0 ||
            Buffer.from(text, 'utf8').length !== blob.length ||
            !Buffer.from(text, 'utf8').equals(blob)) {
            throw new Error('checkpoint has invalid byte encoding');
        }
        const parts = text.split('\n');
        // The canonical body is the fourth line; my canonicalJson output never
        // contains a newline, so trailing newline is a fifth empty element.
        if (parts.length !== 5 || parts[4] !== '') {
            throw new Error('checkpoint has trailing content or is truncated');
        }
        if (parts[0] !== CHECKPOINT_MAGIC) {
            throw new Error('checkpoint has an unknown format');
        }
        if (parts[1] !== String(CHECKPOINT_VERSION)) {
            throw new Error('checkpoint has an unsupported version');
        }
        const body = parts[3];
        if (sha256(body) !== parts[2]) {
            throw new Error('checkpoint digest does not match its content');
        }
        let parsed: any;
        try {
            parsed = JSON.parse(body);
        } catch (e) {
            throw new Error('checkpoint body is not valid JSON');
        }
        if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
            throw new Error('checkpoint body is malformed');
        }
        const keys = Object.keys(parsed).sort().join(',');
        const expectedKeys = [
            'buildID', 'corpus', 'cursor', 'executions', 'features', 'items',
            'planDigest', 'sources', 'stage', 'stages', 'version',
        ].join(',');
        if (keys !== expectedKeys) {
            throw new Error('checkpoint has unexpected fields');
        }
        if (parsed.version !== CHECKPOINT_VERSION) {
            throw new Error('checkpoint has an unsupported version');
        }
        if (parsed.buildID !== buildID) {
            throw new Error('checkpoint was produced for a different build');
        }
        if (!Array.isArray(parsed.stages)) {
            throw new Error('checkpoint plan is malformed');
        }
        const stages: Buffer[][] = [];
        for (const stage of parsed.stages) {
            if (!Array.isArray(stage)) {
                throw new Error('checkpoint plan stage is malformed');
            }
            const inputs: Buffer[] = [];
            for (const encoded of stage) {
                if (typeof encoded !== 'string') {
                    throw new Error('checkpoint plan input is malformed');
                }
                inputs.push(decodeBase64(encoded, 'plan input'));
            }
            stages.push(inputs);
        }
        if (typeof parsed.planDigest !== 'string' ||
            parsed.planDigest !== ReplayController.planDigestFromBuffers(stages)) {
            throw new Error('checkpoint plan digest does not match its plan');
        }
        if (!Number.isInteger(parsed.stage) || parsed.stage < 0 ||
            parsed.stage > stages.length) {
            throw new Error('checkpoint stage is out of range');
        }
        if (!Number.isInteger(parsed.cursor) || parsed.cursor < 0) {
            throw new Error('checkpoint cursor is invalid');
        }
        const pointer = ReplayController.pointerFor(stages, parsed.stage, parsed.cursor);
        if (pointer.stage !== parsed.stage || pointer.cursor !== parsed.cursor) {
            throw new Error('checkpoint cursor does not point at the next item');
        }
        if (!Number.isInteger(parsed.executions) || parsed.executions < 0) {
            throw new Error('checkpoint executions is invalid');
        }
        if (!Array.isArray(parsed.sources)) {
            throw new Error('checkpoint sources are malformed');
        }
        const sources = ReplayController.validateSources(parsed.sources,
            'checkpoint sources');
        if (!Array.isArray(parsed.features)) {
            throw new Error('checkpoint feature set is malformed');
        }
        const features = ReplayController.validateCheckpointFeatures(
            parsed.features, 'checkpoint feature set', sources);
        if (!Array.isArray(parsed.corpus)) {
            throw new Error('checkpoint corpus is malformed');
        }
        const corpus: Buffer[] = [];
        for (const encoded of parsed.corpus) {
            if (typeof encoded !== 'string') {
                throw new Error('checkpoint corpus entry is malformed');
            }
            corpus.push(decodeBase64(encoded, 'corpus entry'));
        }
        if (!Array.isArray(parsed.items)) {
            throw new Error('checkpoint feedback log is malformed');
        }

        const total = ReplayController.planLength(stages);
        if (parsed.executions !== parsed.items.length) {
            throw new Error('checkpoint executions disagree with its feedback log');
        }
        const itemStage = ReplayController.pointerOfExecutions(
            stages, parsed.executions);
        const next = ReplayController.pointerFor(stages, parsed.stage, parsed.cursor);
        if (itemStage.stage !== next.stage || itemStage.cursor !== next.cursor) {
            throw new Error('checkpoint position disagrees with its feedback log');
        }
        if (parsed.executions > total) {
            throw new Error('checkpoint has more executions than plan items');
        }

        const items: CheckpointItem[] = [];
        let running: string[] = [];
        for (let i = 0; i < parsed.items.length; i++) {
            const raw = parsed.items[i];
            const item = ReplayController.validateItem(raw, sources, stages);
            if (item.stage !== ReplayController.pointerOfExecutions(stages, i).stage ||
                item.cursor !== ReplayController.pointerOfExecutions(stages, i).cursor) {
                throw new Error('checkpoint feedback log is out of order');
            }
            if (canonicalJson(item.sources) !== canonicalJson(sources)) {
                throw new Error('checkpoint feedback source binding is inconsistent');
            }
            const cumulative = ReplayController.unionSorted(
                running, ReplayController.validateCheckpointFeatures(
                    item.features, `feedback item ${i}`, sources));
            if (canonicalJson(cumulative) !== canonicalJson(item.features)) {
                throw new Error('checkpoint feedback set is not cumulative');
            }
            running = item.features;
            items.push(item);
        }
        if (canonicalJson(running) !== canonicalJson(features)) {
            throw new Error('checkpoint feature set disagrees with its feedback log');
        }

        // Admitted corpus must be reproducible from the feedback log alone.
        const expectedCorpus: Buffer[] = [];
        const digests = new Set<string>();
        for (const item of items) {
            if (!item.added) {
                continue;
            }
            const digest = sha256(item.input as Buffer);
            if (digests.has(digest)) {
                continue;
            }
            digests.add(digest);
            expectedCorpus.push(item.input as Buffer);
        }
        if (canonicalJson(expectedCorpus.map(buf => sha256(buf))) !==
            canonicalJson(corpus.map(buf => sha256(buf)))) {
            throw new Error('checkpoint corpus disagrees with its feedback log');
        }
        for (const buf of corpus) {
            if (digests.has(sha256(buf)) === false) {
                throw new Error('checkpoint corpus contains an unadmitted input');
            }
        }

        if (canonicalJson(sources) !== canonicalJson(
            ReplayController.sortedSources(sources))) {
            throw new Error('checkpoint sources are not sorted');
        }

        return {
            version: parsed.version,
            buildID: parsed.buildID,
            stages,
            stage: parsed.stage,
            cursor: parsed.cursor,
            executions: parsed.executions,
            features,
            sources,
            corpus,
            items,
        };
    }

    private static validateItem(raw: any, sources: Array<[string, string]>,
                                stages: Buffer[][]): CheckpointItem {
        if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) {
            throw new Error('checkpoint feedback item is malformed');
        }
        const keys = Object.keys(raw).sort().join(',');
        if (keys !== 'added,cursor,features,sources,stage') {
            throw new Error('checkpoint feedback item has unexpected fields');
        }
        if (!Number.isInteger(raw.stage) || raw.stage < 0 ||
            raw.stage >= stages.length ||
            !Number.isInteger(raw.cursor) || raw.cursor < 0 ||
            raw.cursor >= stages[raw.stage].length) {
            throw new Error('checkpoint feedback item position is invalid');
        }
        const features = ReplayController.validateCheckpointFeatures(
            raw.features, 'checkpoint feedback features', sources);
        const itemSources = ReplayController.validateSources(
            raw.sources, 'checkpoint feedback sources');
        if (typeof raw.added !== 'boolean') {
            throw new Error('checkpoint feedback item admission flag is invalid');
        }
        return {
            stage: raw.stage,
            cursor: raw.cursor,
            features,
            sources: itemSources,
            added: raw.added,
            input: stages[raw.stage][raw.cursor],
        };
    }

    private static validateCheckpointFeatures(raw: any, what: string,
                                              sources?: Array<[string, string]>):
        string[] {
        if (!Array.isArray(raw)) {
            throw new Error(`${what} is malformed`);
        }
        const digests = new Map<string, string>();
        if (sources) {
            for (const entry of sources) {
                digests.set(entry[0], entry[1]);
            }
        }
        let previous: string | null = null;
        const result: string[] = [];
        for (const feature of raw) {
            if (typeof feature !== 'string') {
                throw new Error(`${what} contains a non-string feature`);
            }
            let parsed: any;
            try {
                parsed = JSON.parse(feature);
            } catch (e) {
                throw new Error(`${what} contains a malformed feature`);
            }
            if (!Array.isArray(parsed) || parsed.length !== 5 ||
                typeof parsed[0] !== 'string' ||
                typeof parsed[1] !== 'string' ||
                (parsed[2] !== 's' && parsed[2] !== 'f' && parsed[2] !== 'b') ||
                typeof parsed[3] !== 'string' || !/^[0-9]+$/.test(parsed[3]) ||
                typeof parsed[4] !== 'number' || !Number.isInteger(parsed[4]) ||
                parsed[4] < 0) {
                throw new Error(`${what} contains a malformed feature`);
            }
            if (parsed[2] !== 'b' && parsed[4] !== 0) {
                throw new Error(`${what} contains a feature with a non-zero index`);
            }
            if (sources) {
                const expectedDigest = digests.get(parsed[0]);
                if (expectedDigest === undefined || expectedDigest !== parsed[1]) {
                    throw new Error(
                        `${what} binds a feature to an unknown source`);
                }
            }
            if (previous !== null && !(previous < feature)) {
                throw new Error(`${what} is not sorted and deduplicated`);
            }
            previous = feature;
            result.push(feature);
        }
        return result;
    }

    private static validateSources(raw: any, what: string):
        Array<[string, string]> {
        if (!Array.isArray(raw)) {
            throw new Error(`${what} is malformed`);
        }
        const sources: Array<[string, string]> = [];
        let previous: string | null = null;
        for (const entry of raw) {
            if (!Array.isArray(entry) || entry.length !== 2 ||
                typeof entry[0] !== 'string' || typeof entry[1] !== 'string' ||
                !/^[0-9a-f]{64}$/.test(entry[1]) || entry[0].length === 0 ||
                entry[0].split('/').some((part: string) => part === '..' ||
                    part.length === 0)) {
                throw new Error(`${what} contains a malformed entry`);
            }
            if (previous !== null && !(previous < entry[0])) {
                throw new Error(`${what} is not sorted and deduplicated`);
            }
            previous = entry[0];
            sources.push([entry[0], entry[1]]);
        }
        return sources;
    }

    private static pointerOfExecutions(stages: Buffer[][], count: number):
        {stage: number, cursor: number} {
        let remaining = count;
        for (let stage = 0; stage < stages.length; stage++) {
            if (remaining < stages[stage].length) {
                return {stage, cursor: remaining};
            }
            remaining -= stages[stage].length;
        }
        return {stage: stages.length, cursor: 0};
    }

    private static pointerFor(stages: Buffer[][], stage: number, cursor: number):
        {stage: number, cursor: number} {
        let s = stage;
        let c = cursor;
        while (s < stages.length && c >= stages[s].length) {
            s += 1;
            c = 0;
        }
        if (s >= stages.length) {
            return {stage: stages.length, cursor: 0};
        }
        return {stage: s, cursor: c};
    }

    private static planLength(stages: Buffer[][]): number {
        let total = 0;
        for (const stage of stages) {
            total += stage.length;
        }
        return total;
    }

    // -- restore replay ----------------------------------------------------

    private async replayPrefix(data: CheckpointData) {
        const liveSources = ReplayController.sortedSources(
            await this.worker.whenReady());
        // The fresh worker's real source set (relative path + pre-instrumentation
        // source digest) must match the archive exactly. This also covers an
        // archive with zero executions, where there would otherwise be no
        // replay to compare against.
        if (!ReplayController.sourcesEqual(liveSources,
            ReplayController.sortedSources(data.sources))) {
            throw new Error('target sources changed since the checkpoint');
        }
        this.sources = data.sources.map(entry => [entry[0], entry[1]] as [string, string]);
        let running: string[] = [];
        for (let index = 0; index < data.items.length; index++) {
            const item = data.items[index];
            const input = Buffer.from(data.stages[item.stage][item.cursor]);
            const seq = this.worker.nextSeq();
            const nonce = crypto.randomBytes(16).toString('hex');
            const message = await this.worker.sendReplay(
                this.session, seq, nonce, input, this.generation, this.timeoutMs());
            if (!ReplayController.matchesIdentity(message, this.session,
                this.generation, seq, nonce)) {
                throw new Error('replay worker answered a different request');
            }
            if (message.type !== WorkerMessageType.REPLAY_RESULT) {
                throw new Error(`replay rebuild failed: ${message.message}`);
            }
            const observedSources = ReplayController.sortedSources(
                message.sources || []);
            if (!ReplayController.sourcesEqual(this.sources, observedSources)) {
                throw new Error('target source set changed since checkpoint');
            }
            const observed = ReplayController.validateFeatureList(
                message.features, this.sources);
            const cumulative = ReplayController.unionSorted(running, observed);
            if (canonicalJson(cumulative) !== canonicalJson(item.features)) {
                throw new Error(
                    'replay rebuild produced a different coverage feature set');
            }
            if (canonicalJson(item.sources) !== canonicalJson(this.sources)) {
                throw new Error('replay checkpoint source binding is inconsistent');
            }
            running = cumulative;
            // Preserve the recorded execution count and corpus; the prefix is
            // not counted again, and no archive coverage is injected as if it
            // had been executed now.
            this.executions += 1;
            this.stage = item.stage;
            this.cursor = item.cursor;
            this.items.push({
                stage: item.stage,
                cursor: item.cursor,
                features: item.features.slice(),
                sources: this.sources.map(entry => [entry[0], entry[1]]),
                added: item.added,
            });
            this.advancePointer();
        }
        if (canonicalJson(running) !== canonicalJson(data.features)) {
            throw new Error('replay rebuild did not reach the archived feature set');
        }
        // Rebuild the real Corpus from admitted inputs only.
        const digests = new Set<string>();
        for (const item of data.items) {
            if (!item.added) {
                continue;
            }
            const input = Buffer.from(data.stages[item.stage][item.cursor]);
            const digest = sha256(input);
            if (digests.has(digest)) {
                continue;
            }
            digests.add(digest);
            this.corpus.putBuffer(input);
        }
        this.corpusDigests = digests;
        this.stage = data.stage;
        this.cursor = data.cursor;
        this.items = data.items.map(item => ({
            stage: item.stage,
            cursor: item.cursor,
            features: item.features.slice(),
            sources: item.sources.map(entry => [entry[0], entry[1]]),
            added: item.added,
        }));
        this.features = data.features.slice();
    }
}
