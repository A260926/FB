import type { BufferGeometry } from 'three';
import type { BVHOptions, MeshBVH } from '../index.js';

export interface PLOCThreadStats {

	threadId: number;
	candidateEvaluations: number;
	mergedPairs: number;

}

export interface PLOCRoundStats {

	round: number;
	activeClusters: number;
	mergedPairs: number;
	candidateEvaluations: number;
	nextActiveClusters: number;
	rootIndex?: number;

}

export interface PLOCStats {

	workerCount: number;
	rounds: PLOCRoundStats[];
	activeClustersPerRound: number[];
	candidateEvaluations: number;
	mergedPairs: number;
	threads: Record<number, PLOCThreadStats>;
	threadStats: Record<number, PLOCThreadStats>;

}

export interface NodePLOCGenerateResult {

	bvh: MeshBVH & {
		version: 1;
		roots: Array<ArrayBuffer | SharedArrayBuffer>;
		index: ArrayBufferView | null;
		indirectBuffer: Uint16Array | Uint32Array | null;
	};
	stats: PLOCStats;

}

export declare class NodePLOCWorker {

	readonly workerCount: number;
	running: boolean;

	constructor( workers?: number | { workers?: number } );

	generate( geometry: BufferGeometry, options?: BVHOptions ): Promise<NodePLOCGenerateResult>;
	dispose(): Promise<void>;

}

export default NodePLOCWorker;
