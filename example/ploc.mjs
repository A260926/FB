import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { BufferGeometry, Float32BufferAttribute, Ray, Vector3, DoubleSide } from 'three';
import { NodePLOCWorker } from '../src/workers/NodePLOCWorker.js';

const DEFAULT_WIDTH = 160;
const DEFAULT_HEIGHT = 120;
const DEFAULT_SEGMENTS = 64;
const DEFAULT_WORKERS = 4;

// a wavy grid of quads: 2 * segments^2 triangles (8192 at the default 64 segments)
export function createPreviewGeometry( segments = DEFAULT_SEGMENTS ) {

	const vertices = [];
	const uvs = [];
	for ( let y = 0; y <= segments; y ++ ) {

		for ( let x = 0; x <= segments; x ++ ) {

			const u = x / segments;
			const v = y / segments;
			const px = ( u - 0.5 ) * 2;
			const py = ( v - 0.5 ) * 2;
			const pz = 0.25 * Math.sin( px * 6 ) * Math.cos( py * 5 ) + 0.08 * Math.sin( ( px + py ) * 14 );
			vertices.push( px, py, pz );
			uvs.push( u, v );

		}

	}

	const indices = [];
	for ( let y = 0; y < segments; y ++ ) {

		for ( let x = 0; x < segments; x ++ ) {

			const a = y * ( segments + 1 ) + x;
			const b = a + 1;
			const c = a + segments + 1;
			const d = c + 1;
			indices.push( a, b, d, a, d, c );

		}

	}

	const geometry = new BufferGeometry();
	geometry.setAttribute( 'position', new Float32BufferAttribute( vertices, 3 ) );
	geometry.setAttribute( 'uv', new Float32BufferAttribute( uvs, 2 ) );
	geometry.setIndex( indices );
	return geometry;

}

// pinhole camera used for the preview
export function createPreviewCamera( width, height ) {

	const origin = new Vector3( 0, - 2.2, 1.35 );
	const lookAt = new Vector3( 0, 0, 0.05 );
	const forward = lookAt.clone().sub( origin ).normalize();
	const right = new Vector3().crossVectors( forward, new Vector3( 0, 0, 1 ) ).normalize();
	const up = new Vector3().crossVectors( right, forward ).normalize();
	const halfH = Math.tan( Math.PI / 3.4 / 2 );
	const halfW = halfH * ( width / height );
	return { origin, forward, right, up, halfW, halfH };

}

export function computePixelRay( x, y, width, height, camera ) {

	const ndcX = ( ( x + 0.5 ) / width ) * 2 - 1;
	const ndcY = 1 - ( ( y + 0.5 ) / height ) * 2;
	const direction = camera.forward.clone()
		.addScaledVector( camera.right, ndcX * camera.halfW )
		.addScaledVector( camera.up, ndcY * camera.halfH )
		.normalize();
	return new Ray( camera.origin.clone(), direction );

}

const _light = /* @__PURE__ */ new Vector3( 0.4, - 0.6, 1 ).normalize();

// shades a hit with a single directional light and a height based color ramp
export function shadeHit( hit, rayDirection, target ) {

	const normal = hit.face.normal;
	const sign = normal.dot( rayDirection ) > 0 ? - 1 : 1;
	const diffuse = Math.max( 0, sign * normal.dot( _light ) );
	const shade = 0.2 + 0.8 * diffuse;
	const t = Math.min( 1, Math.max( 0, hit.point.z * 1.6 + 0.5 ) );
	target[ 0 ] = Math.round( 255 * shade * ( 0.25 + 0.55 * t ) );
	target[ 1 ] = Math.round( 255 * shade * ( 0.45 + 0.25 * t ) );
	target[ 2 ] = Math.round( 255 * shade * ( 0.85 - 0.45 * t ) );
	return target;

}

export function shadeBackground( y, height, target ) {

	const g = Math.round( 30 + 60 * ( y / height ) );
	target[ 0 ] = g;
	target[ 1 ] = g;
	target[ 2 ] = Math.min( 255, g + 30 );
	return target;

}

/**
 * Renders a windowless preview of the preview grid through a PLOC tree built with
 * the worker pool. Returns the RGB pixel data along with the BVH and build stats.
 */
export async function renderPreview( options = {} ) {

	const {
		width = DEFAULT_WIDTH,
		height = DEFAULT_HEIGHT,
		segments = DEFAULT_SEGMENTS,
		workers = DEFAULT_WORKERS,
	} = options;

	const geometry = createPreviewGeometry( segments );
	const worker = new NodePLOCWorker( { workers } );
	let bvh, stats;
	try {

		const result = await worker.generate( geometry, { targetLeafSize: 6 } );
		bvh = result.bvh;
		stats = result.stats;

	} finally {

		await worker.dispose();

	}

	const camera = createPreviewCamera( width, height );
	const data = new Uint8Array( width * height * 3 );
	const pixel = [ 0, 0, 0 ];
	for ( let y = 0; y < height; y ++ ) {

		for ( let x = 0; x < width; x ++ ) {

			const ray = computePixelRay( x, y, width, height, camera );
			const hit = bvh.raycastFirst( ray, DoubleSide );
			const offset = ( y * width + x ) * 3;
			if ( hit ) {

				shadeHit( hit, ray.direction, pixel );

			} else {

				shadeBackground( y, height, pixel );

			}

			data[ offset + 0 ] = pixel[ 0 ];
			data[ offset + 1 ] = pixel[ 1 ];
			data[ offset + 2 ] = pixel[ 2 ];

		}

	}

	return { width, height, data, bvh, stats, geometry };

}

// encodes RGB pixel data as a binary PPM image
export function encodePPM( width, height, data ) {

	const header = Buffer.from( `P6\n${ width } ${ height }\n255\n`, 'ascii' );
	return Buffer.concat( [ header, Buffer.from( data.buffer, data.byteOffset, data.byteLength ) ] );

}

async function main() {

	const outPath = process.argv[ 2 ];
	if ( ! outPath ) {

		console.error( 'usage: node example/ploc.mjs <output.ppm>' );
		process.exit( 1 );

	}

	const { width, height, data, stats } = await renderPreview();
	const directory = path.dirname( outPath );
	if ( directory && directory !== '.' ) {

		fs.mkdirSync( directory, { recursive: true } );

	}

	fs.writeFileSync( outPath, encodePPM( width, height, data ) );
	console.log(
		`PLOC preview written to ${ outPath } (${ width }x${ height }, ` +
		`rounds=${ stats.rounds.length - 1 }, candidateEvaluations=${ stats.candidateEvaluations }, ` +
		`mergedPairs=${ stats.mergedPairs })`,
	);

}

if ( process.argv[ 1 ] && import.meta.url === pathToFileURL( process.argv[ 1 ] ).href ) {

	main().catch( error => {

		console.error( error );
		process.exit( 1 );

	} );

}
