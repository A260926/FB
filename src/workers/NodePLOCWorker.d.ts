import { BufferGeometry } from 'three';
import { MeshBVH, BVHOptions } from '../index.js';

export interface PLOCThreadStats {
	candidateEvaluations: number;
	mergedPairs: number;
}

export interface PLOCStats {
	/** Active cluster count at the start of every aggregation round, final 1 appended. */
	rounds: Array<number>;
	/** Merged active-position pairs of every round. */
	roundPairs: Array<Array<[ number, number ]>>;
	/** Total union area evaluations performed across all threads. */
	candidateEvaluations: number;
	/** Total cluster pairs merged across all threads. */
	mergedPairs: number;
	/** Per thread work counters, indexed by thread id. */
	threads: Array<PLOCThreadStats>;
}

export interface NodePLOCWorkerGenerateResult {
	bvh: MeshBVH;
	stats: PLOCStats;
}

export class NodePLOCWorker {

	readonly running: boolean;
	readonly disposed: boolean;
	readonly workerCount: number;
	maxWorkerCount: number;

	constructor( options?: { workers?: number } | number );
	generate( geometry: BufferGeometry, options?: BVHOptions ): Promise<NodePLOCWorkerGenerateResult>;
	dispose(): Promise<void>;

}
