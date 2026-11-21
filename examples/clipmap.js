#!/usr/bin/env node
'use strict';
/*
 * examples/clipmap.js
 *
 * Drives the real production Render inside VoxelSpace.html through the
 * headless viewer, backed by the new procedural clipmap module.  Renders
 * three small frames:
 *   1. the procedural terrain at the first camera position,
 *   2. the same view after the camera moves (clipmap windows follow it),
 *   3. the moved view after a sparse source edit raises a ridge.
 * The frames are written side by side as one binary P6 PPM at argv[2].
 *
 * Run: node examples/clipmap.js output.ppm
 * Node builtins only; no package manager and no image downloads.
 */

const fs = require('node:fs');
const path = require('node:path');
const {TerrainSource, TerrainClipmap} = require('../terrain-clipmap.js');
const {runViewer} = require('../task_support/viewer.js');

const outPath = process.argv[2];
if (!outPath) {
    process.stderr.write('usage: node examples/clipmap.js <output.ppm>\n');
    process.exit(1);
}

// Deterministic procedural world sampled only at integer grid points.
function baseTerrain(x, y) {
    const wave = Math.sin(x * 0.11) * 6 + Math.cos(y * 0.09) * 6 +
                 Math.sin((x + y) * 0.05) * 4;
    const height = wave;
    // 0xAABBGGRR word; derive a simple green/brown gradient from the height.
    const level = Math.max(0, Math.min(255, Math.round(90 + wave * 7)));
    const r = Math.round(40 + wave * 2) & 255;
    const g = level;
    const b = (60 - wave * 3) & 255;
    const color = (0xff000000 | (b << 16) | (g << 8) | r) >>> 0;
    return {height, color};
}

const WIDTH = 72;
const HEIGHT = 48;
const DISTANCE = 220;

// Render still references map.width/height for the periodic constants even on
// the clipmap path, so supply a minimal stand-in map.
const map = {
    width: 1024, height: 1024, shift: 10,
    altitude: new Uint8Array(1024 * 1024),
    color: new Uint32Array(1024 * 1024)
};

function renderFrame(camera, configure) {
    return runViewer({
        map,
        width: WIDTH,
        height: HEIGHT,
        camera,
        background: 0xff90d0ff,
        configure
    });
}

// One procedural source for the whole example; a single clipmap view is
// attached to the production renderer and reused across every frame.
const source = new TerrainSource(baseTerrain);
const clip = new TerrainClipmap(source, {side: 17, levels: 9});

let session = null;
let attachStats = null;
function renderFrame(camera) {
    const result = runViewer({
        map,
        width: WIDTH,
        height: HEIGHT,
        camera,
        background: 0xff90d0ff,
        session,
        configure: (globals) => {
            globals.AttachTerrainClipmap(source, clip);
            // Captured here: this is the moveTo that actually follows the
            // camera.  Render's own moveTo finds the same window and reads
            // nothing, so the set-difference count is the one below.
            attachStats = clip.stats();
        }
    });
    session = result.session;
    return result;
}

const cameraA = {x: 0, y: 0, height: 30, angle: 0, horizon: 20, distance: DISTANCE};
const frameA = renderFrame(cameraA);
const statsA = clip.stats();
const attachReadsA = attachStats.lastReads;

// Move the camera: the clipmap windows follow and only the set difference is
// sampled; the ring never rebuilds a whole-world array.
const cameraB = {x: 24, y: -12, height: 30, angle: 0.4, horizon: 20, distance: DISTANCE};
const frameB = renderFrame(cameraB);
const statsB = clip.stats();
const moveReads = attachStats.lastReads;

// Sparse edit: raise a ridge just ahead of the moved camera, inside its
// finest-level window, so bilinear reconstruction shows a clear silhouette.
// Edits use the unwrapped world coordinates, independent of any periodic base.
const ridge = [];
for (let x = 10; x <= 40; x++) {
    ridge.push({x, y: -16, height: 30, color: 0xff303080});
}
source.edit(ridge);
const editStats = clip.stats();
const frameC = renderFrame(cameraB);
const statsC = clip.stats();

let editedPixels = 0;
for (let i = 0; i < frameB.color.length; i++) {
    if (frameB.color[i] !== frameC.color[i]) editedPixels++;
}

function toRgb(word) {
    return [word & 255, (word >>> 8) & 255, (word >>> 16) & 255];
}

// Compose the three frames side by side into one P6 PPM.
const frames = [frameA.color, frameB.color, frameC.color];
const totalWidth = WIDTH * frames.length;
const pixels = Buffer.alloc(totalWidth * HEIGHT * 3);
for (let f = 0; f < frames.length; f++) {
    const buf = frames[f];
    for (let y = 0; y < HEIGHT; y++) {
        for (let x = 0; x < WIDTH; x++) {
            const [r, g, b] = toRgb(buf[y * WIDTH + x]);
            const o = ((y * totalWidth) + (f * WIDTH + x)) * 3;
            pixels[o] = r;
            pixels[o + 1] = g;
            pixels[o + 2] = b;
        }
    }
}

const header = Buffer.from(`P6\n${totalWidth} ${HEIGHT}\n255\n`, 'ascii');
fs.mkdirSync(path.dirname(path.resolve(outPath)), {recursive: true});
fs.writeFileSync(outPath, Buffer.concat([header, pixels]));

process.stdout.write([
    `wrote ${outPath} (${totalWidth}x${HEIGHT}, ${header.length + pixels.length} bytes)`,
    `frame A initial fills=${attachReadsA} cumulativeReads=${statsA.reads} resident=${statsA.resident}`,
    `frame B camera moveTo new samples=${moveReads} cumulativeReads=${statsB.reads}`,
    `ridge sparse edit refreshed reads=${editStats.lastReads}`,
    `frame C pixels changed by the edit=${editedPixels} cumulativeReads=${statsC.reads}`,
    `source subscribers=${source.subscriberCount()}`
].join('\n') + '\n');
