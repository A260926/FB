import { Worker } from 'node:worker_threads';
import { Box3, BufferAttribute } from 'three';
import { MeshBVH } from '../core/MeshBVH.js';
import { PLOC, SKIP_GENERATION, DEFAULT_OPTIONS } from '../core/Constants.js';
import { ensureIndex, getRootPrimitiveRanges } from '../core/build/geometryUtils.js';
import { generateIndirectBuffer } from '../core/GeometryBVH.js';
import { buildPLOCPackedTreeParallel } from '../core/build/PLOCBuilder.js';

const WORKER_URL = new URL( './plocNode.worker.js', import.meta.url );

function normalizeWorkerCount( value ) {

	let count = value;
	if ( value && typeof value === 'object' ) {

		count = value.workers ?? value.workerCount ?? value.maxWorkers ?? value.count ?? value.threads;

	}

	if ( count === undefined ) {

		count = 4;

	}

	if ( typeof count !== 'number' || ! Number.isInteger( count ) || count < 1 || count > 16 ) {

		throw new RangeError( 'NodePLOCWorker: workers must be an integer from 1 to 16.' );

	}

	return count;

}

function splitArray( array, workerCount ) {

	const count = Math.max( 1, Math.min( array.length, workerCount ) );
	const base = Math.floor( array.length / count );
	const remainder = array.length % count;
	const chunks = [];
	let offset = 0;
	for ( let i = 0; i < count; i ++ ) {

		const size = base + ( i < remainder ? 1 : 0 );
		chunks.push( array.slice( offset, offset + size ) );
		offset += size;

	}

	return chunks;

}

/**
 * Node.js worker-pool interface for deterministic parallel PLOC builds.
 *
 * `generate(geometry, options)` resolves to `{ bvh, stats }`. Candidate neighbor
 * evaluation and parent bounds merging are both executed in plocNode.worker.js
 * threads. The resulting MeshBVH uses the same native packed roots as the
 * synchronous `new MeshBVH(geometry, { strategy: PLOC })` path.
 */
export class NodePLOCWorker {

	/**
	 * @param {number|{workers?:number}} [workers=4] Worker count, 1 through 16.
	 */
	constructor( workers = 4 ) {

		this.workerCount = normalizeWorkerCount( workers );

		/** Backwards-friendly aliases for callers expecting constructor metadata. */
		this.workers = this.workerCount;
		this.maxWorkerCount = this.workerCount;
		this.options = { workers: this.workerCount };
		this.running = false;
		this._disposed = false;
		this._entries = [];
		this._requestId = 0;
		this._taskStats = null;
		this._disposePromise = null;

		for ( let i = 0; i < this.workerCount; i ++ ) {

			this._spawnWorker();

		}

	}

	_spawnWorker() {

		const worker = new Worker( WORKER_URL, { type: 'module' } );
		const entry = {
			index: this._entries.length,
			worker,
			threadId: worker.threadId,
			pending: new Map(),
			ready: false,
			readyPromise: null,
			resolveReady: null,
			rejectReady: null,
		};
		entry.readyPromise = new Promise( ( resolve, reject ) => {

			entry.resolveReady = resolve;
			entry.rejectReady = reject;

		} );

		worker.on( 'message', message => this._handleMessage( entry, message ) );
		worker.on( 'error', error => this._handleWorkerError( entry, error ) );
		worker.on( 'exit', code => {

			if ( code !== 0 && ! this._disposed ) {

				this._handleWorkerError(
					entry,
					new Error( `plocNode.worker exited with code ${ code }.` ),
				);

			}

		} );

		entry.readyPromise.catch( () => {} );
		this._entries.push( entry );

	}

	_handleMessage( entry, message ) {

		if ( message && message.type === 'ready' && ! entry.ready ) {

			entry.ready = true;
			entry.resolveReady?.( message );
			return;

		}

		const pending = entry.pending.get( message?.id );
		if ( ! pending ) {

			return;

		}

		entry.pending.delete( message.id );
		if ( message.ok === false ) {

			const error = new Error( message.error?.message || 'plocNode.worker task failed.' );
			error.name = message.error?.name || 'Error';
			error.stack = message.error?.stack || error.stack;
			pending.reject( error );

			// A worker-side task failure invalidates the in-flight round. Close the
			// pool so sibling chunks cannot publish into a subsequent generation.
			this._handleWorkerError( entry, error );

		} else {

			pending.resolve( message );

		}

	}

	_rejectEntryPending( entry, error ) {

		if ( entry.rejectReady && ! entry.ready ) {

			entry.rejectReady( error );

		}

		for ( const pending of entry.pending.values() ) {

			pending.reject( error );

		}

		entry.pending.clear();

	}

	_handleWorkerError( failedEntry, error ) {

		if ( this._disposed ) {

			return;

		}

		this._taskError = error;
		for ( const entry of this._entries ) {

			this._rejectEntryPending( entry, error );

		}

		// A thread-level failure leaves the pool in an unknown state. Close this
		// generation so no caller can observe or reuse half-written cluster state.
		this.dispose().catch( () => {} );

	}

	_request( entry, type, payload = {} ) {

		if ( this._disposed ) {

			return Promise.reject( new Error( 'NodePLOCWorker: Worker has been disposed.' ) );

		}

		const id = ++ this._requestId;
		return new Promise( ( resolve, reject ) => {

			entry.pending.set( id, { resolve, reject } );
			try {

				entry.worker.postMessage( { id, type, ...payload } );

			} catch ( error ) {

				entry.pending.delete( id );
				reject( error );

			}

		} );

	}

	async _readyAll() {

		await Promise.all( this._entries.map( entry => entry.readyPromise ) );

	}

	_createExecutor() {

		return async ( operation, payload ) => {

			if ( operation === 'candidates' ) {

				const chunks = splitArray( payload.indices, this._entries.length );
				const responses = await Promise.all( chunks.map( async ( indices, i ) => {

					if ( indices.length === 0 ) {

						return { proposals: [], candidateEvaluations: 0, threadId: this._entries[ i ].threadId };

					}

					const entry = this._entries[ i ];
					const result = await this._request( entry, 'candidates', {
						clusters: payload.clusters,
						indices,
					} );
					if ( result.stats ) {

						this._taskStats.threads[ entry.threadId ] = { ...result.stats };

					} else {

						this._taskStats.threads[ entry.threadId ].candidateEvaluations += result.candidateEvaluations;

					}

					return result;

				} ) );

				return {
					proposals: responses.flatMap( response => response.proposals ),
					candidateEvaluations: responses.reduce(
						( sum, response ) => sum + response.candidateEvaluations,
						0,
					),
				};

			}

			if ( operation === 'merges' ) {

				const chunks = splitArray( payload.pairs, this._entries.length );
				const responses = await Promise.all( chunks.map( async ( pairs, i ) => {

					if ( pairs.length === 0 ) {

						return { merges: [], mergedPairs: 0 };

					}

					const entry = this._entries[ i ];
					const result = await this._request( entry, 'merges', { pairs } );
					if ( result.stats ) {

						this._taskStats.threads[ entry.threadId ] = { ...result.stats };

					} else {

						this._taskStats.threads[ entry.threadId ].mergedPairs += result.mergedPairs;

					}

					return result;

				} ) );

				return {
					merges: responses.flatMap( response => response.merges ),
					mergedPairs: responses.reduce( ( sum, response ) => sum + response.mergedPairs, 0 ),
				};

			}

			throw new Error( `NodePLOCWorker: Unknown PLOC operation "${ operation }".` );

		};

	}

	async _runGenerate( geometry, options ) {

		if ( ! geometry || ! geometry.isBufferGeometry ) {

			throw new Error( 'NodePLOCWorker: generate() requires a BufferGeometry.' );

		}

		if ( ! geometry.attributes.position ) {

			geometry.setAttribute( 'position', new BufferAttribute( new Float32Array( 0 ), 3 ) );

		}

		const buildOptions = {
			...DEFAULT_OPTIONS,
			...options,
			strategy: PLOC,
			verbose: options.verbose ?? false,
			[ SKIP_GENERATION ]: true,
		};

		await this._readyAll();
		if ( this._disposed ) {

			throw new Error( 'NodePLOCWorker: Worker has been disposed.' );

		}

		this._taskStats = {
			workerCount: this.workerCount,
			threads: {},
		};
		for ( const entry of this._entries ) {

			this._taskStats.threads[ entry.threadId ] = {
				threadId: entry.threadId,
				id: entry.threadId,
				workerIndex: entry.index,
				candidateEvaluations: 0,
				mergedPairs: 0,
			};
			await this._request( entry, 'resetStats' );

		}

		const bvh = new MeshBVH( geometry, buildOptions );
		const ranges = getRootPrimitiveRanges( geometry, options.range ?? null, bvh.primitiveStride || 3 );
		if ( buildOptions.indirect ) {

			bvh._indirectBuffer = generateIndirectBuffer( ranges, buildOptions.useSharedArrayBuffer );
			bvh.resolvePrimitiveIndex = i => bvh._indirectBuffer[ i ];

		} else {

			ensureIndex( geometry, buildOptions );

		}

		const { stats: buildStats } = await buildPLOCPackedTreeParallel(
			bvh,
			buildOptions,
			this._createExecutor(),
		);

		if ( buildOptions.setBoundingBox && ! geometry.boundingBox ) {

			geometry.boundingBox = bvh.getBoundingBox( new Box3() );

		}

		const indexAttribute = geometry.getIndex();
		const serializedView = {
			version: 1,
			roots: bvh._roots,
			index: indexAttribute ? indexAttribute.array : null,
			indirectBuffer: bvh._indirectBuffer,
		};
		Object.assign( bvh, serializedView );

		const activeClusterHistory = buildStats.activeClustersPerRound.length ?
			[
				buildStats.activeClustersPerRound[ 0 ],
				...buildStats.rounds.map( round => round.nextActiveClusters ),
			] :
			[];
		const stats = {
			...buildStats,
			...this._taskStats,
			activeClusters: buildStats.activeClustersPerRound,
			activeClusterHistory,
			clusterCounts: buildStats.activeClustersPerRound,
			roundCounts: buildStats.activeClustersPerRound,
			roundDetails: buildStats.rounds,
			threadStats: this._taskStats.threads,
			byThreadId: this._taskStats.threads,
			workerStats: this._taskStats.threads,
			threadCounts: this._taskStats.threads,
			byWorkerIndex: Object.fromEntries( this._entries.map( entry => [
				entry.index,
				this._taskStats.threads[ entry.threadId ],
			] ) ),
			threadList: Object.values( this._taskStats.threads ),
			workers: this._taskStats.threads,
		};

		// Also expose each row directly as stats[realNodeThreadId] for callers that
		// expect thread ids at the top level of the statistics object.
		for ( const [ threadId, row ] of Object.entries( this._taskStats.threads ) ) {

			stats[ threadId ] = row;

		}

		return { bvh, stats };

	}

	/**
	 * Generates a PLOC MeshBVH and build statistics.
	 * @param {import('three').BufferGeometry} geometry
	 * @param {object} [options]
	 * @returns {Promise<{bvh:MeshBVH,stats:object}>}
	 */
	generate( geometry, options = {} ) {

		if ( this._disposed || this.running ) {

			const message = this._disposed ?
				'NodePLOCWorker: Worker has been disposed.' :
				'NodePLOCWorker: A generate task is already running.';
			const rejection = Promise.reject( new Error( message ) );
			// Prevent an unhandled-rejection diagnostic if the caller attaches late.
			rejection.catch( () => {} );
			return rejection;

		}

		this.running = true;
		const promise = this._runGenerate( geometry, options );
		// Ensure callers that race dispose() still never observe an unhandled rejection
		// solely because they have not attached their own rejection handler yet.
		promise.catch( () => {} );
		const finish = () => {

			this.running = false;

		};
		promise.then( finish, finish );
		return promise;

	}

	/**
	 * Terminates every Node worker thread and rejects pending work.
	 * @returns {Promise<void>}
	 */
	dispose() {

		if ( this._disposePromise ) {

			return this._disposePromise;

		}

		this._disposed = true;
		const disposedError = new Error( 'NodePLOCWorker: dispose() was called before generation completed.' );
		for ( const entry of this._entries ) {

			this._rejectEntryPending( entry, disposedError );

		}

		this._disposePromise = Promise.allSettled(
			this._entries.map( entry => entry.worker.terminate() ),
		).then( () => {

			this._entries = [];

		} );
		return this._disposePromise;

	}

}

export default NodePLOCWorker;
