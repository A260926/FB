export enum WorkerMessageType {
    RESULT,
    CRASH,
    REPLAY_RESULT,
    REPLAY_ERROR,
    READY,
}
export interface WorkerMessage {
    type: WorkerMessageType,
    coverage: number,
    error: number
}

// A replay result carries the identity of the exact request it answers as
// well as the real Istanbul feature set observed after the target returned.
export interface ReplayResultMessage {
    type: WorkerMessageType.REPLAY_RESULT,
    session: string,
    generation: number,
    seq: number,
    nonce: string,
    features: string[],
    sources: Array<[string, string]>,
    coverage: number
}

export interface ReplayErrorMessage {
    type: WorkerMessageType.REPLAY_ERROR,
    session?: string,
    generation?: number,
    seq?: number,
    nonce?: string,
    message: string
}

export interface ReadyMessage {
    type: WorkerMessageType.READY
}

export enum ManageMessageType {
    WORK,
    STOP,
    REPLAY,
}
export interface ManagerMessage {
    type: ManageMessageType
    buf: Buffer
    session?: string
    generation?: number
    seq?: number
    nonce?: string
}
