#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import {
	BufferGeometry,
	DoubleSide,
	Float32BufferAttribute,
	PerspectiveCamera,
	Raycaster,
	Vector3,
} from 'three';
import { MeshBVH, PLOC } from '../src/index.js';

const outputPath = process.argv[ 2 ];
if ( ! outputPath ) {

	console.error( 'Usage: node example/ploc.mjs <output.ppm>' );
	process.exit( 2 );

}

const width = 240;
const height = 180;
const columns = 42;
const rows = 32;
const spacing = .38;

function waveZ( x, y ) {

	return .55 * Math.sin( x * .72 ) * Math.cos( y * .63 ) +
		.18 * Math.sin( ( x + y ) * 1.35 );

}

// Two triangles per wavy grid cell. Vertices are expanded per face so the sample
// also exercises non-indexed geometry through the synchronous PLOC builder.
const positions = [];
for ( let row = 0; row < rows; row ++ ) {

	for ( let column = 0; column < columns; column ++ ) {

		const x0 = ( column - columns / 2 ) * spacing;
		const x1 = x0 + spacing;
		const y0 = ( row - rows / 2 ) * spacing;
		const y1 = y0 + spacing;
		const p00 = [ x0, y0, waveZ( x0, y0 ) ];
		const p10 = [ x1, y0, waveZ( x1, y0 ) ];
		const p01 = [ x0, y1, waveZ( x0, y1 ) ];
		const p11 = [ x1, y1, waveZ( x1, y1 ) ];
		positions.push( ...p00, ...p10, ...p01 );
		positions.push( ...p10, ...p11, ...p01 );

	}

}

const geometry = new BufferGeometry();
geometry.setAttribute( 'position', new Float32BufferAttribute( positions, 3 ) );

// The preview is generated entirely by the new bottom-up PLOC packed tree.
const bvh = new MeshBVH( geometry, {
	strategy: PLOC,
	targetLeafSize: 5,
	maxDepth: 40,
	verbose: false,
} );

const camera = new PerspectiveCamera( 42, width / height, .1, 100 );
camera.position.set( 1.4, - 10.5, 6.2 );
camera.lookAt( 0, 0, .15 );
camera.updateMatrixWorld( true );

const raycaster = new Raycaster();
const lightDirection = new Vector3( .45, - .35, .82 ).normalize();
const pixels = Buffer.alloc( width * height * 3 );

function hash01( value ) {

	const x = Math.sin( value * 127.1 + 311.7 ) * 43758.5453123;
	return x - Math.floor( x );

}

for ( let y = 0; y < height; y ++ ) {

	for ( let x = 0; x < width; x ++ ) {

		const ndcX = ( ( x + .5 ) / width ) * 2 - 1;
		const ndcY = 1 - ( ( y + .5 ) / height ) * 2;
		raycaster.setFromCamera( { x: ndcX, y: ndcY }, camera );

		const pixelIndex = ( y * width + x ) * 3;
		const hit = bvh.raycastFirst( raycaster.ray, DoubleSide );
		if ( ! hit ) {

			const glow = Math.max( 0, 1 - Math.hypot( ndcX * .75, ndcY ) );
			pixels[ pixelIndex + 0 ] = 10 + Math.round( 18 * glow );
			pixels[ pixelIndex + 1 ] = 16 + Math.round( 24 * glow );
			pixels[ pixelIndex + 2 ] = 30 + Math.round( 42 * glow );
			continue;

		}

		const normal = hit.face?.normal || new Vector3( 0, 0, 1 );
		const diffuse = Math.max( 0, normal.dot( lightDirection ) );
		const lift = .32 + .68 * diffuse;
		const checker = (
			Math.floor( ( hit.point.x + 20 ) / ( spacing * 2 ) ) +
			Math.floor( ( hit.point.y + 20 ) / ( spacing * 2 ) )
		) % 2;
		const faceTint = .86 + .14 * hash01( hit.faceIndex );
		const baseR = ( checker ? 226 : 72 ) * faceTint;
		const baseG = ( checker ? 132 : 178 ) * faceTint;
		const baseB = ( checker ? 72 : 224 ) * faceTint;

		// A subtle distance fog keeps the PPM preview readable without a window.
		const fog = Math.min( .42, Math.max( 0, ( hit.distance - 9 ) * .08 ) );
		pixels[ pixelIndex + 0 ] = Math.round( baseR * lift * ( 1 - fog ) + 24 * fog );
		pixels[ pixelIndex + 1 ] = Math.round( baseG * lift * ( 1 - fog ) + 30 * fog );
		pixels[ pixelIndex + 2 ] = Math.round( baseB * lift * ( 1 - fog ) + 48 * fog );

	}

}

const header = `P6\n${ width } ${ height }\n255\n`;
fs.mkdirSync( path.dirname( path.resolve( outputPath ) ), { recursive: true } );
fs.writeFileSync( outputPath, Buffer.concat( [ Buffer.from( header, 'ascii' ), pixels ] ) );
console.log( `PLOC preview wrote ${ outputPath } (${ width }x${ height }, ${ geometry.attributes.position.count / 3 } faces)` );
