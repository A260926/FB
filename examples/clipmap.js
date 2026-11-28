'use strict';

// Headless clipmap example. Builds a procedural TerrainSource + TerrainClipmap,
// drives the real VoxelSpace.html production Render through runViewer, and
// writes a single P6 PPM containing three frames side by side:
//   1. initial procedural terrain
//   2. the camera after moving
//   3. the same view after a sparse terrain edit (a raised block)
//
// Usage: node examples/clipmap.js <output.ppm>

const fs = require('node:fs');
const path = require('node:path');
const {TerrainSource} = require('../terrain-clipmap.js');
const {runViewer} = require('../task_support/viewer.js');

const outPath = process.argv[2];
if (!outPath) {
    console.error('usage: node examples/clipmap.js <output.ppm>');
    process.exit(1);
}

const W = 80;
const H = 50;

// Deterministic procedural terrain: smooth analytic hills with a color that
// is a pure function of the integer world grid point.
const source = new TerrainSource(function (x, y) {
    const height = 12 * Math.sin(x * 0.18) + 10 * Math.cos(y * 0.15) +
                   6 * Math.sin((x + y) * 0.11);
    const r = mod256(40 + x * 3);
    const g = mod256(70 + y * 5);
    const b = mod256(120 + (x - y) * 2);
    return {height: height, color: ((255 << 24) | (b << 16) | (g << 8) | r) >>> 0};
});
function mod256(v) {
    return ((Math.round(v) % 256) + 256) % 256;
}

// side 33 -> R 16, margin 14; levels 6 -> b_last = 14 * 32 = 448, which keeps
// every sample of the 220-deep view frustum resident.
// The TerrainClipmap itself is built inside the production viewer through its
// public installClipmap entry; this Node side only owns the source.
// A map is still required by the shared globals even though clipmap sampling
// never touches it.
const map = {
    width: 256, height: 256, shift: 8,
    altitude: new Uint8Array(256 * 256),
    color: new Uint32Array(256 * 256)
};

function renderFrame(camera, session) {
    return runViewer({
        width: W,
        height: H,
        map: map,
        camera: Object.assign({}, camera),
        background: 0xffe09090,
        session: session,
        configure: session ? undefined : function (c) {
            // First frame: install the real procedural source through the
            // production lifecycle entry, which builds and subscribes the
            // clipmap view inside the viewer.
            c.terrain.side = 33;
            c.terrain.levels = 6;
            c.installClipmap(source);
        }
    });
}

// Frame 1: initial position.
const cam1 = {x: 32, y: 40, height: 30, angle: 0.6, horizon: 10, distance: 220};
const result1 = renderFrame(cam1);
const frame1 = result1.color;
const session = result1.session;
const view = session.terrain.view;

// Frame 2: move the camera (the production Render re-centers the clipmap).
const cam2 = {x: 46, y: 52, height: 30, angle: 0.6, horizon: 10, distance: 220};
const frame2 = renderFrame(cam2, session).color;

// Frame 3: raise a sparse block of grid points in front of the new camera,
// then render again. The edit flows through the subscription into the view.
const edits = [];
for (let dx = -2; dx <= 2; dx++) {
    for (let dy = -2; dy <= 2; dy++) {
        const x = Math.round(cam2.x) + dx;
        const y = Math.round(cam2.y) - 6 + dy;
        const current = source.read(x, y);
        edits.push({x: x, y: y, height: current.height + 35, color: 0xff00c0ff});
    }
}
source.edit(edits);
const frame3 = renderFrame(cam2, session).color;

// Compose the three frames horizontally into one P6 PPM.
const outW = W * 3;
const outH = H;
const rgb = Buffer.alloc(outW * outH * 3);
function putFrame(frame, offsetX) {
    for (let y = 0; y < H; y++) {
        for (let x = 0; x < W; x++) {
            const c = frame[y * W + x] >>> 0;
            const o = ((y * outW) + (offsetX + x)) * 3;
            rgb[o] = c & 255;
            rgb[o + 1] = (c >>> 8) & 255;
            rgb[o + 2] = (c >>> 16) & 255;
        }
    }
}
putFrame(frame1, 0);
putFrame(frame2, W);
putFrame(frame3, W * 2);

const header = Buffer.from('P6\n' + outW + ' ' + outH + '\n255\n', 'ascii');
fs.mkdirSync(path.dirname(outPath), {recursive: true});
fs.writeFileSync(outPath, Buffer.concat([header, rgb]));

function nonSky(frame) {
    return frame.reduce((n, c) => n + ((c >>> 0) === 0xffe09090 ? 0 : 1), 0);
}
console.log('wrote', outPath, outW + 'x' + outH, 'PPM');
console.log('frame ground pixels  initial=' + nonSky(frame1) +
            ' moved=' + nonSky(frame2) + ' edited=' + nonSky(frame3));
console.log('clipmap stats', JSON.stringify(view.stats()));
console.log('source subscribers', source.subscriberCount());
