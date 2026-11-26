'use strict';

/*
 * examples/generate-example.js
 *
 * Generates examples/perspective-roll.ppm: a procedurally built stepped
 * terrain with a rolled camera and a tall pillar occluding geometry behind
 * it. Only Node.js built-ins and terrain-renderer.js are used.
 *
 * Run: node examples/generate-example.js
 */

const fs = require('node:fs');
const path = require('node:path');
const renderer = require(path.join(__dirname, '..', 'terrain-renderer.js'));

function encode(r, g, b) {
    // Same little-endian uint32 convention as VoxelSpace map.color.
    return (0xff000000 | (b << 16) | (g << 8) | r) >>> 0;
}

function buildExampleMap() {
    const w = 96;
    const h = 64;
    const altitude = new Uint8Array(w * h);
    const color = new Uint32Array(w * h);

    const bands = [
        encode(52, 78, 132),
        encode(66, 106, 168),
        encode(78, 142, 132),
        encode(100, 168, 98),
        encode(180, 176, 88),
        encode(198, 140, 68)
    ];

    let seed = 2463534242;
    function rand() {
        seed = (seed * 1664525 + 1013904223) >>> 0;
        return seed / 4294967296;
    }

    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            const i = x + w * y;
            // Gentle block terraces with deterministic per-block variation.
            const bx = Math.floor(x / 8);
            const by = Math.floor(y / 8);
            let level = (bx + 2 * by) % 3;

            // Straight staircase climbing with +X through y 31..33.
            if (y >= 31 && y <= 33) {
                level = Math.min(5, Math.max(0, Math.floor((x - 6) / 4)));
            }

            altitude[i] = level * 2;
            // Slight per-cell brightness jitter so adjacent columns read.
            const jitter = Math.floor(rand() * 14) - 7;
            const base = bands[level];
            const r = Math.max(0, Math.min(255, (base & 0xff) + jitter));
            const g = Math.max(0, Math.min(255, ((base >> 8) & 0xff) + jitter));
            const b = Math.max(0, Math.min(255, ((base >> 16) & 0xff) + jitter));
            color[i] = encode(r, g, b);
        }
    }

    function block(x0, x1, y0, y1, h, c) {
        for (let yy = y0; yy <= y1; yy++) {
            for (let xx = x0; xx <= x1; xx++) {
                altitude[xx + w * yy] = h;
                color[xx + w * yy] = c;
            }
        }
    }

    // Chunky red pillar in the foreground (2x2 columns).
    block(24, 25, 31, 32, 18, encode(232, 74, 74));

    // Long low wall behind, crossing the same sight line so the red pillar
    // is seen to cut a gap out of it.
    for (let y = 27; y <= 37; y++) {
        altitude[31 + w * y] = 8;
        color[31 + w * y] = encode(96, 190, 214);
    }

    // Taller yellow pillar directly behind the red one: its base is occluded
    // while its crown rises above the red pillar.
    block(30, 31, 31, 32, 24, encode(244, 206, 84));

    // Off-axis stepped tower enriching the rolled silhouette.
    block(38, 39, 42, 43, 13, encode(216, 216, 226));

    return { width: w, height: h, altitude: altitude, color: color };
}

function writePPM(file, width, height, rgbaColors) {
    const header = Buffer.from(`P6\n${width} ${height}\n255\n`, 'ascii');
    const rgb = Buffer.alloc(width * height * 3);
    for (let i = 0; i < width * height; i++) {
        const c = rgbaColors[i] >>> 0;
        rgb[i * 3 + 0] = c & 0xff;         // R (low byte in 0xAABBGGRR)
        rgb[i * 3 + 1] = (c >> 8) & 0xff;  // G
        rgb[i * 3 + 2] = (c >> 16) & 0xff; // B
    }
    fs.writeFileSync(file, Buffer.concat([header, rgb]));
}

function main() {
    const map = buildExampleMap();
    const width = 320;
    const height = 200;

    const camera = {
        position: [3, 32.5, 9],
        yaw: Math.PI / 2, // look along +X down the staircase
        pitch: 0.08,      // nearly level, slight downward angle
        roll: 0.5,        // obvious right-hand positive roll
        fovY: (70 * Math.PI) / 180,
        near: 0.1,
        far: 120,
        background: encode(168, 196, 228)
    };

    const frame = renderer.renderPerspective(map, camera, width, height);
    const outFile = path.join(__dirname, 'perspective-roll.ppm');
    writePPM(outFile, width, height, frame.color);

    let sky = 0;
    let pillar = 0;
    for (let i = 0; i < frame.color.length; i++) {
        if (frame.depth[i] === Infinity) sky++;
        if (frame.color[i] === encode(232, 74, 74)) pillar++;
    }
    console.log(`wrote ${outFile} (${width}x${height}), sky=${sky}, redPillarPixels=${pillar}`);
}

main();
