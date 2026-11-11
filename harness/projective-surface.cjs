'use strict';

// Samples a small non-planar NURBS surface after an exact projective
// transformation and writes it as a Wavefront OBJ mesh with vertices (v),
// unit normals (vn) computed from the analytic first partial derivatives,
// and faces (f). No plotting libraries involved.
//
// Usage: node harness/projective-surface.cjs <output.obj>

const fs = require('node:fs');
const nurbs = require('../nurbs');

const outPath = process.argv[2];
if (!outPath) {
  console.error('Usage: node harness/projective-surface.cjs <output.obj>');
  process.exit(1);
}

// A non-planar 4x4 control net.
const points = [];
for (let i = 0; i < 4; i++) {
  const row = [];
  for (let j = 0; j < 4; j++) {
    row.push([i - 1.5, j - 1.5, Math.sin(i * 1.1) * Math.cos(j * 0.9)]);
  }
  points.push(row);
}

const surface = nurbs({
  points: points,
  degree: 2,
  boundary: 'clamped'
});

// A genuinely projective 4x4 (column-major); the last row is
// (0.2, -0.1, 0.3, 1), so the transform has a non-trivial denominator.
const m = [
  1, 0, 0, 0.2,
  0, 1, 0, -0.1,
  0, 0, 1, 0.3,
  1, -2, 0.5, 1
];
surface.projectiveTransform(m);

// Sample an (N + 1) x (M + 1) grid over the domain.
const N = 12;
const M = 12;
const domain = surface.domain;
const du = surface.evaluator([1, 0]);
const dv = surface.evaluator([0, 1]);

const lines = [];
const normals = [];
for (let b = 0; b <= M; b++) {
  for (let a = 0; a <= N; a++) {
    const u = domain[0][0] + (domain[0][1] - domain[0][0]) * a / N;
    const v = domain[1][0] + (domain[1][1] - domain[1][0]) * b / M;
    const p = surface.evaluate([], u, v);
    const pu = du([], u, v);
    const pv = dv([], u, v);

    // Unit normal from the two parametric partial derivatives.
    let nx = pu[1] * pv[2] - pu[2] * pv[1];
    let ny = pu[2] * pv[0] - pu[0] * pv[2];
    let nz = pu[0] * pv[1] - pu[1] * pv[0];
    const len = Math.hypot(nx, ny, nz);
    if (!(len > 0) || !Number.isFinite(len)) {
      throw new Error('Degenerate surface normal at (u, v) = (' + u + ', ' + v + ')');
    }
    nx /= len;
    ny /= len;
    nz /= len;

    lines.push('v ' + p[0] + ' ' + p[1] + ' ' + p[2]);
    normals.push('vn ' + nx + ' ' + ny + ' ' + nz);
  }
}
lines.push(...normals);

const index = (a, b) => b * (N + 1) + a + 1; // OBJ indices are 1-based
for (let b = 0; b < M; b++) {
  for (let a = 0; a < N; a++) {
    const i00 = index(a, b);
    const i10 = index(a + 1, b);
    const i01 = index(a, b + 1);
    const i11 = index(a + 1, b + 1);
    lines.push('f ' + i00 + '//' + i00 + ' ' + i10 + '//' + i10 + ' ' + i11 + '//' + i11);
    lines.push('f ' + i00 + '//' + i00 + ' ' + i11 + '//' + i11 + ' ' + i01 + '//' + i01);
  }
}

fs.writeFileSync(outPath, lines.join('\n') + '\n');
console.log('Wrote ' + (N + 1) * (M + 1) + ' vertices, ' + normals.length + ' normals, ' + (2 * N * M) + ' faces to ' + outPath);
