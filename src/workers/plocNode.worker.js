import { parentPort, threadId } from 'node:worker_threads';
import {
	evaluateCandidateBatch,
	mergeClusterPairBatch,
} from '../core/build/PLOCBuilder.js';

const stats = {
	threadId,
	candidateEvaluations: 0,
	mergedPairs: 0,
};

function reply( message ) {

	parentPort.postMessage( message );

}

parentPort.on( 'message', message => {

	const { id, type } = message;
	try {

		if ( type === 'readyCheck' ) {

			reply( { id, ok: true, threadId, type: 'ready' } );
			return;

		}

		if ( type === 'resetStats' ) {

			stats.candidateEvaluations = 0;
			stats.mergedPairs = 0;
			reply( { id, ok: true, threadId, stats: { ...stats } } );
			return;

		}

		if ( type === 'candidates' ) {

			const result = evaluateCandidateBatch( {
				clusters: message.clusters,
				indices: message.indices,
			} );
			stats.candidateEvaluations += result.candidateEvaluations;
			reply( {
				id,
				ok: true,
				threadId,
				proposals: result.proposals,
				candidateEvaluations: result.candidateEvaluations,
				stats: { ...stats },
			} );
			return;

		}

		if ( type === 'merges' ) {

			const result = mergeClusterPairBatch( { pairs: message.pairs } );
			stats.mergedPairs += result.mergedPairs;
			reply( {
				id,
				ok: true,
				threadId,
				merges: result.merges,
				mergedPairs: result.mergedPairs,
				stats: { ...stats },
			} );
			return;

		}

		throw new Error( `plocNode.worker: Unknown message type "${ type }".` );

	} catch ( error ) {

		reply( {
			id,
			ok: false,
			threadId,
			error: {
				name: error?.name || 'Error',
				message: error?.message || String( error ),
				stack: error?.stack || null,
			},
		} );

	}

} );

reply( { type: 'ready', threadId } );
