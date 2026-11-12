// Generates an SVG wireframe comparing the triangulation before and after an interior
// point removal. Run with: node sanity/remove-example.mjs [output.svg]
import {writeFileSync} from 'node:fs';
import Delaunator from '../index.js';

const points = [
    [0, 0], [100, 0], [100, 100], [0, 100],
    [28, 31], [14, 36], [16, 15], [33, 25], [31, 25],
    [55, 60], [70, 40], [45, 75], [60, 15], [80, 80], [20, 55]
];
const removedIndex = 4;

const before = Delaunator.from(points);
const edgesBefore = meshEdges(before);
const starBefore = starEdges(before, removedIndex);

const after = Delaunator.from(points);
after.removePoint(removedIndex);
const edgesAfter = meshEdges(after);

const width = 760;
const panel = 340;
const margin = 40;
const scale = (panel - 2 * margin) / 100;

function project(p, offsetX) {
    return [offsetX + margin + p[0] * scale, margin + p[1] * scale];
}

function meshEdges(d) {
    const edges = new Set();
    const key = (u, v) => u < v ? `${u}:${v}` : `${v}:${u}`;
    for (let t = 0; t < d.triangles.length; t += 3) {
        for (let j = 0; j < 3; j++) {
            edges.add(key(d.triangles[t + j], d.triangles[t + (j + 1) % 3]));
        }
    }
    return [...edges].map(k => k.split(':').map(Number));
}

function starEdges(d, index) {
    const edges = new Set();
    for (let t = 0; t < d.triangles.length; t += 3) {
        const ids = [d.triangles[t], d.triangles[t + 1], d.triangles[t + 2]];
        if (!ids.includes(index)) continue;
        for (let j = 0; j < 3; j++) edges.add([ids[j], ids[(j + 1) % 3]]);
    }
    return [...edges];
}

function line(u, v, offsetX, cls) {
    const [x1, y1] = project(points[u], offsetX);
    const [x2, y2] = project(points[v], offsetX);
    return `<line class="${cls}" x1="${x1.toFixed(1)}" y1="${y1.toFixed(1)}" x2="${x2.toFixed(1)}" y2="${y2.toFixed(1)}"/>`;
}

function panelSvg(offsetX, title, edges, highlight, showRemoved) {
    const parts = [];
    parts.push(`<g>`);
    for (const [u, v] of edges) parts.push(line(u, v, offsetX, 'edge'));
    if (highlight) for (const [u, v] of highlight) parts.push(line(u, v, offsetX, 'star'));
    for (let i = 0; i < points.length; i++) {
        if (!showRemoved && i === removedIndex) continue;
        const [cx, cy] = project(points[i], offsetX);
        const cls = i === removedIndex ? 'point removed' : 'point';
        parts.push(`<circle class="${cls}" cx="${cx.toFixed(1)}" cy="${cy.toFixed(1)}" r="${i === removedIndex ? 5 : 3}"/>`);
    }
    parts.push(`<text class="title" x="${offsetX + panel / 2}" y="24">${title}</text>`);
    parts.push(`</g>`);
    return parts.join('\n');
}

const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${panel}" viewBox="0 0 ${width} ${panel}" font-family="sans-serif">
<style>
  .edge { stroke: #888; stroke-width: 1; }
  .star { stroke: #d33; stroke-width: 2.5; }
  .point { fill: #226; stroke: #fff; stroke-width: 1; }
  .point.removed { fill: #d33; }
  .title { text-anchor: middle; font-size: 16px; fill: #222; }
</style>
<rect width="100%" height="100%" fill="white"/>
${panelSvg(0, `before: removePoint(${removedIndex}) — star neighborhood in red`, edgesBefore, starBefore, true)}
${panelSvg(panel + margin * 2, 'after: cavity retriangulated locally', edgesAfter, null, false)}
</svg>
`;

const out = process.argv[2] || new URL('./remove-example.svg', import.meta.url);
writeFileSync(out, svg);
console.log(`SVG wireframe written to ${out}`);
