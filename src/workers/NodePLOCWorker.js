/** @import { BufferGeometry } from 'three' */
import { Worker } from 'node:worker_threads';
import { Box3 } from 'three';
import { MeshBVH } from '../core/MeshBVH.js';
import { DEFAULT_OPTIONS, SKIP_GENERATION, PLOC } from '../core/Constants.js';
import { ensureIndex, getRootPrimitiveRanges } from '../core/build/geometryUtils.js';
import { generateIndirectBuffer } from '../core/GeometryBVH.js';
import {
	PLOC_NEIGHBOR_WINDOW,
	preparePLOCState,
	compactActiveClusters,
	proposeNeighbors,
	computeRoundPairs,
	gatherPairData,
	mergeClusterBounds,
	applyMergeResults,
	layoutPLOCTree,
	buildTreePLOC,
	createPLOCStats,
} from '../core/build/PLOCBuilder.js';

const DEFAULT_WORKER_COUNT = 4;
const MAX_WORKER_COUNT = 16;

/**
 * Node.js worker-thread driver for bottom-up PLOC BVH construction. The neighbor
 * candidate evaluation and the parent cluster bounds merging of every aggregation
 * round are distributed over a pool of threads from `plocNode.worker.js`, while the
 * deterministic pairing and compaction happen on the coordinator. The resulting tree
 * is byte identical to a synchronous `MeshBVH` build with `strategy: PLOC` for the
 * same geometry and options, regardless of the worker count.
 *
 * The instance cannot run overlapping `generate` calls. Call `dispose` to terminate
 * the thread pool.
 *
 * @note It's best to reuse a single instance of this class to avoid the overhead of
 * instantiating new threads.
 */
export class NodePLOCWorker {

	/**
	 * @param {Object|number} [options] - options object or worker count
	 * @param {number} [options.workers=4] - number of threads, between 1 and 16
	 */
	constructor( options = {} ) {

		if ( typeof options === 'number' ) {

			options = { workers: options };

		}

		const workers = options.workers === undefined ? DEFAULT_WORKER_COUNT : options.workers;
		if ( ! Number.isInteger( workers ) || workers < 1 || workers > MAX_WORKER_COUNT ) {

			throw new Error( `NodePLOCWorker: "workers" must be an integer between 1 and ${ MAX_WORKER_COUNT }.` );

		}

		/**
		 * Number of threads in the pool.
		 * @type {number}
		 */
		this.maxWorkerCount = workers;

		/**
		 * Flag indicating whether or not a BVH is already being generated.
		 * @type {boolean}
		 */
		this.running = false;

		this._disposed = false;
		this._workers = [];
		this._readyPromise = null;
		this._task = null;
		this._nextTaskId = 0;

	}

	/**
	 * Number of live threads in the pool.
	 * @type {number}
	 * @readonly
	 */
	get workerCount() {

		return this._workers.length;

	}

	/**
	 * Whether `dispose` has been called.
	 * @type {boolean}
	 * @readonly
	 */
	get disposed() {

		return this._disposed;

	}

	/**
	 * Generates a `MeshBVH` for the given geometry using PLOC clustering distributed
	 * over the thread pool. Resolves with the generated BVH - a regular `MeshBVH`
	 * instance whose packed roots can be used with every query function - and the
	 * build statistics. Rejects if the instance is already running a build or has
	 * been disposed, and rejects if any worker reports an error.
	 *
	 * @param {BufferGeometry} geometry
	 * @param {Object} [options] - Same options accepted by the `MeshBVH` constructor.
	 * @returns {Promise<{ bvh: MeshBVH, stats: Object }>}
	 */
	generate( geometry, options = {} ) {

		if ( this._disposed ) {

			return Promise.reject( new Error( 'NodePLOCWorker: Worker has been disposed.' ) );

		}

		if ( this.running ) {

			return Promise.reject( new Error( 'NodePLOCWorker: Already running job.' ) );

		}

		this.running = true;
		const promise = this._run( geometry, options );

		// "finally" would return a second promise that rejects unhandled when the build fails
		const onDone = () => {

			this.running = false;

		};

		promise.then( onDone, onDone );
		return promise;

	}

	/**
	 * Terminates the thread pool. Any in-flight `generate` promise is rejected and no
	 * pending promises or live threads are left behind. Safe to call multiple times.
	 *
	 * @returns {Promise<void>} resolves once all threads have exited
	 */
	dispose() {

		this._disposed = true;
		this._cancelTask( new Error( 'NodePLOCWorker: Worker has been disposed.' ) );
		return this._terminateWorkers();

	}

	async _run( geometry, options ) {

		const task = {
			id: ++ this._nextTaskId,
			cancelled: false,
			pending: new Set(),
		};
		this._task = task;

		try {

			await this._ensureWorkers();
			if ( task.cancelled || this._disposed ) {

				await this._terminateWorkers();
				throw new Error( 'NodePLOCWorker: Task was cancelled.' );

			}

			return await this._build( geometry, options, task );

		} finally {

			if ( this._task === task ) {

				this._task = null;

			}

		}

	}

	// spawns the thread pool lazily and waits for every thread to signal readiness
	_ensureWorkers() {

		if ( this._readyPromise ) {

			return this._readyPromise;

		}

		const readyPromises = [];
		try {

			for ( let i = 0; i < this.maxWorkerCount; i ++ ) {

				const worker = new Worker( new URL( './plocNode.worker.js', import.meta.url ) );
				worker.unref();
				this._workers.push( worker );
				readyPromises.push( new Promise( ( resolve, reject ) => {

					const cleanup = () => {

						worker.off( 'message', onMessage );
						worker.off( 'error', onFailure );
						worker.off( 'exit', onFailure );

					};

					const onMessage = data => {

						if ( data && data.type === 'ready' ) {

							cleanup();
							resolve();

						}

					};

					const onFailure = arg => {

						cleanup();
						reject( arg instanceof Error ? arg : new Error( `NodePLOCWorker: worker ${ i } failed to start.` ) );

					};

					worker.on( 'message', onMessage );
					worker.once( 'error', onFailure );
					worker.once( 'exit', onFailure );

				} ) );

			}

		} catch ( error ) {

			// a partially spawned pool must not leak threads
			this._terminateWorkers();
			throw error;

		}

		this._readyPromise = Promise.all( readyPromises ).then( () => {

			this._workers.forEach( ( worker, index ) => {

				worker._plocAlive = true;
				worker.on( 'error', error => {

					worker._plocAlive = false;
					if ( this._workers.includes( worker ) ) {

						this._handleWorkerFailure( new Error( `NodePLOCWorker: worker ${ index } error: ${ error && error.message || error }` ) );

					}

				} );

				worker.on( 'exit', code => {

					worker._plocAlive = false;
					if ( this._workers.includes( worker ) ) {

						this._handleWorkerFailure( new Error( `NodePLOCWorker: worker ${ index } exited unexpectedly (code ${ code }).` ) );

					}

				} );

			} );

		} );

		// a failed pool is dropped so a later "generate" spawns a fresh one
		this._readyPromise.catch( () => {

			this._terminateWorkers();

		} );

		return this._readyPromise;

	}

	// rejects the in-flight task (if any) so "generate" settles instead of hanging
	_cancelTask( error ) {

		const task = this._task;
		if ( ! task ) {

			return;

		}

		task.cancelled = true;
		for ( const pending of task.pending ) {

			pending.reject( error );

		}

		task.pending.clear();

	}

	// a worker error or unexpected exit winds down the current task and drops the
	// whole pool so no half-written state or zombie thread is reused
	_handleWorkerFailure( error ) {

		this._cancelTask( error );
		this._terminateWorkers();

	}

	_terminateWorkers() {

		const workers = this._workers;
		this._workers = [];
		this._readyPromise = null;

		// listeners stay attached so a not-yet-ready pool still settles its readiness
		// promise via the resulting "exit" events; retired workers are ignored by the
		// "includes" guard in the persistent handlers
		return Promise.all( workers.map( worker => worker.terminate().catch( () => undefined ) ) ).then( () => undefined );

	}

	// posts one message to a worker and waits for its single response, tracked on the
	// task so it can be rejected when the task is wound down
	_postTask( worker, message, task, transfer ) {

		return new Promise( ( resolve, reject ) => {

			if ( task.cancelled || ! worker._plocAlive ) {

				reject( new Error( 'NodePLOCWorker: Task was cancelled.' ) );
				return;

			}

			const pending = { reject: null };
			const cleanup = () => {

				worker.off( 'message', onMessage );
				task.pending.delete( pending );

			};

			const onMessage = data => {

				if ( ! data || data.taskId !== message.taskId ) {

					return;

				}

				cleanup();
				if ( data.type === 'error' ) {

					reject( new Error( `NodePLOCWorker: ${ data.message }` ) );

				} else {

					resolve( data );

				}

			};

			pending.reject = error => {

				cleanup();
				reject( error );

			};

			task.pending.add( pending );
			worker.on( 'message', onMessage );

			try {

				worker.postMessage( message, transfer );

			} catch ( error ) {

				pending.reject( error );

			}

		} );

	}

	async _build( geometry, options, task ) {

		if ( ! geometry.isBufferGeometry ) {

			throw new Error( 'NodePLOCWorker: Only BufferGeometries are supported.' );

		}

		const posAttr = geometry.getAttribute( 'position' );
		if ( ! posAttr ) {

			throw new Error( 'NodePLOCWorker: Geometry must have a "position" attribute.' );

		}

		if (
			posAttr.isInterleavedBufferAttribute ||
			geometry.index && geometry.index.isInterleavedBufferAttribute
		) {

			throw new Error( 'NodePLOCWorker: InterleavedBufferAttribute are not supported for the geometry attributes.' );

		}

		// mirror the setup of a synchronous MeshBVH build so the results match exactly
		const opts = {
			...DEFAULT_OPTIONS,
			...options,
			strategy: PLOC,
		};

		let indirectBuffer = null;
		if ( opts.indirect ) {

			const ranges = getRootPrimitiveRanges( geometry, opts.range, 3 );
			indirectBuffer = generateIndirectBuffer( ranges, opts.useSharedArrayBuffer );

		} else {

			ensureIndex( geometry, opts );

		}

		const bvh = new MeshBVH( geometry, { ...opts, [ SKIP_GENERATION ]: true } );
		bvh._indirectBuffer = indirectBuffer;

		const rootRanges = bvh.getRootRanges( opts.range );
		const firstRange = rootRanges[ 0 ];
		const lastRange = rootRanges[ rootRanges.length - 1 ];
		const fullRange = {
			offset: firstRange.offset,
			count: lastRange.offset + lastRange.count - firstRange.offset,
		};

		const primitiveBounds = new Float32Array( 6 * fullRange.count );
		primitiveBounds.offset = fullRange.offset;
		bvh.computePrimitiveBounds( fullRange.offset, fullRange.count, primitiveBounds );

		const stats = createPLOCStats();
		stats.threads = this._workers.map( () => ( { candidateEvaluations: 0, mergedPairs: 0 } ) );

		const roots = [];
		for ( let i = 0, l = rootRanges.length; i < l; i ++ ) {

			roots.push( await this._buildRoot( bvh, primitiveBounds, rootRanges[ i ], opts, fullRange, stats, task ) );

		}

		bvh._roots = roots;

		if ( ! geometry.boundingBox && opts.setBoundingBox ) {

			geometry.boundingBox = bvh.getBoundingBox( new Box3() );

		}

		return { bvh, stats };

	}

	// builds a single packed root range, distributing the candidate evaluation and the
	// parent bounds merging of every round over the thread pool. Pairing and
	// compaction only run once all proposals / merges of the round have completed, so
	// no thread ever observes a half-written cluster.
	async _buildRoot( bvh, primitiveBounds, range, opts, fullRange, stats, task ) {

		const { offset, count } = range;
		if ( count < 1 ) {

			return buildTreePLOC( bvh, primitiveBounds, offset, count, opts, fullRange );

		}

		const workers = this._workers;
		const workerCount = workers.length;
		const state = preparePLOCState( primitiveBounds, offset, count );

		while ( state.activeCount > 1 ) {

			if ( task.cancelled ) {

				throw new Error( 'NodePLOCWorker: Task was cancelled.' );

			}

			stats.rounds.push( state.activeCount );
			const m = state.activeCount;
			const activeData = compactActiveClusters( state );
			const bounds = activeData.bounds;
			const minFace = activeData.minFace;

			// propose phase: every thread evaluates the candidates for a contiguous
			// slice of the frozen active order
			const best = new Int32Array( m );
			const proposeJobs = [];
			for ( let w = 0; w < workerCount; w ++ ) {

				const start = Math.floor( w * m / workerCount );
				const end = Math.floor( ( w + 1 ) * m / workerCount );
				if ( end <= start ) {

					continue;

				}

				const lo = Math.max( 0, start - PLOC_NEIGHBOR_WINDOW );
				const hi = Math.min( m, end + PLOC_NEIGHBOR_WINDOW );
				const message = {
					type: 'propose',
					taskId: task.id,
					windowOffset: lo,
					totalCount: m,
					start,
					end,
					bounds: bounds.slice( lo * 6, hi * 6 ),
					minFace: minFace.slice( lo, hi ),
				};

				proposeJobs.push(
					this._postTask( workers[ w ], message, task, [ message.bounds.buffer, message.minFace.buffer ] )
						.then( data => {

							best.set( data.best, start );
							stats.candidateEvaluations += data.evaluations;
							stats.threads[ w ].candidateEvaluations += data.evaluations;

						} ),
				);

			}

			await Promise.all( proposeJobs );

			// pairing is deterministic and happens only after every proposal is in
			const pairs = computeRoundPairs( best, m );
			stats.roundPairs.push( pairs );

			// merge phase: the real parent bounds unions are computed by the threads
			const pairData = gatherPairData( bounds, minFace, pairs );
			const pairCount = pairs.length;
			const parentBounds = new Float64Array( pairCount * 6 );
			const parentMinFace = new Int32Array( pairCount );
			const mergeJobs = [];
			for ( let w = 0; w < workerCount; w ++ ) {

				const start = Math.floor( w * pairCount / workerCount );
				const end = Math.floor( ( w + 1 ) * pairCount / workerCount );
				if ( end <= start ) {

					continue;

				}

				const message = {
					type: 'merge',
					taskId: task.id,
					count: end - start,
					leftBounds: pairData.leftBounds.slice( start * 6, end * 6 ),
					rightBounds: pairData.rightBounds.slice( start * 6, end * 6 ),
					leftMinFace: pairData.leftMinFace.slice( start, end ),
					rightMinFace: pairData.rightMinFace.slice( start, end ),
				};

				mergeJobs.push(
					this._postTask(
						workers[ w ],
						message,
						task,
						[
							message.leftBounds.buffer,
							message.rightBounds.buffer,
							message.leftMinFace.buffer,
							message.rightMinFace.buffer,
						],
					).then( data => {

						parentBounds.set( data.bounds, start * 6 );
						parentMinFace.set( data.minFace, start );
						stats.mergedPairs += data.mergedPairs;
						stats.threads[ w ].mergedPairs += data.mergedPairs;

					} ),
				);

			}

			await Promise.all( mergeJobs );

			// publish the new clusters only after every merge has completed
			applyMergeResults( state, pairs, parentBounds, parentMinFace );

		}

		stats.rounds.push( state.activeCount );
		state.rootId = state.active[ 0 ];
		return layoutPLOCTree( bvh, state, offset, count, opts, fullRange );

	}

}
