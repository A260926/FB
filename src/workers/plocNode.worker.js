import { parentPort } from 'node:worker_threads';
import { proposeNeighbors, mergeClusterBounds } from '../core/build/PLOCBuilder.js';

// Worker thread entry for NodePLOCWorker. The coordinator owns the clustering state
// and sends self contained slices of the frozen round data; this thread only performs
// the pure neighbor-candidate and parent-bounds computations and reports how much
// work it did so the counts accumulate where the computation happens.
parentPort.on( 'message', message => {

	if ( ! message || typeof message !== 'object' ) {

		return;

	}

	const { type, taskId } = message;
	try {

		if ( type === 'propose' ) {

			const { bounds, minFace, windowOffset, totalCount, start, end } = message;
			const best = new Int32Array( end - start );
			const evaluations = proposeNeighbors( bounds, minFace, windowOffset, totalCount, start, end, best );
			parentPort.postMessage(
				{ type: 'proposeResult', taskId, best, evaluations },
				[ best.buffer ],
			);

		} else if ( type === 'merge' ) {

			const { leftBounds, rightBounds, leftMinFace, rightMinFace, count } = message;
			const bounds = new Float64Array( count * 6 );
			const minFace = new Int32Array( count );
			mergeClusterBounds( leftBounds, rightBounds, leftMinFace, rightMinFace, count, bounds, minFace );
			parentPort.postMessage(
				{ type: 'mergeResult', taskId, bounds, minFace, mergedPairs: count },
				[ bounds.buffer, minFace.buffer ],
			);

		}

	} catch ( error ) {

		parentPort.postMessage( {
			type: 'error',
			taskId,
			message: error && error.message ? error.message : String( error ),
		} );

	}

} );

// signal that the script has loaded
parentPort.postMessage( { type: 'ready' } );
