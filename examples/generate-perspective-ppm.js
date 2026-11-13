/*
 * examples/generate-perspective-ppm.js
 *
 * Renders one perspective frame of the procedural stepped terrain with a
 * rolled camera and a tall pillar occluding cells behind it, then writes a
 * binary PPM (P6). Only JavaScript built-ins are used. Run with:
 *
 *     node examples/generate-perspective-ppm.js [output.ppm]
 */
"use strict";

var path = require("path");
var fs = require("fs");
var TerrainRenderer = require(path.join(__dirname, "..", "terrain-renderer"));

var W = 64, H = 48;
var altitude = new Array(W * H);
var color = new Uint32Array(W * H);

for (var y = 0; y < H; y++) {
    for (var x = 0; x < W; x++) {
        var i = x + W * y;
        var level = Math.floor(y / 6) + Math.floor(x / 12);
        altitude[i] = level;
        var shade = 60 + level * 18;
        var r = Math.min(255, shade);
        var g = Math.min(255, shade + 40);
        var b = Math.min(255, shade + 20);
        color[i] = 0xff000000 | (r << 16) | (g << 8) | b;
    }
}

// Tall pillar near the centre line so it hides the stair band behind it.
var pillarX = 32, pillarY = 24;
altitude[pillarX + W * pillarY] += 14;
color[pillarX + W * pillarY] = 0xffffd040;

var map = { width: W, height: H, altitude: altitude, color: color };

// Camera placed in front of the pillar, looking toward it with a clear roll.
var camera = {
    position: [32, 8, 18],
    yaw: 0,
    pitch: -0.15,
    roll: 0.6,
    fovY: Math.PI / 2,
    near: 0.1,
    far: 120,
    background: 0xff203040
};

var width = 120, height = 80;
var frame = TerrainRenderer.renderPerspective(map, camera, width, height);

// Convert the 0xAARRGGBB words to packed RGB bytes for the PPM.
var rgb = new Uint8Array(width * height * 3);
for (var p = 0; p < width * height; p++) {
    var word = frame.color[p];
    rgb[p * 3 + 0] = (word >>> 16) & 0xff; // R
    rgb[p * 3 + 1] = (word >>> 8) & 0xff;  // G
    rgb[p * 3 + 2] = word & 0xff;          // B
}

var header = Buffer.from("P6\n" + width + " " + height + "\n255\n", "ascii");
var body = Buffer.from(rgb);
var out = process.argv[2] || path.join(__dirname, "perspective-roll.ppm");
fs.writeFileSync(out, Buffer.concat([header, body]));
console.log("wrote " + out + " (" + width + "x" + height + ", roll=" + camera.roll + ")");
