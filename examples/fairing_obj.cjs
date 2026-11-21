'use strict';

//
// Fairing example: drag a few samples on a non-planar, positively weighted
// surface and write the faired surface as a Wavefront OBJ file.
//
// Usage:
//   node examples/fairing_obj.cjs out.obj
//
// The output contains v / vn / f records only, with one normal per vertex and
// triangles for the whole parameter domain. Non-zero exit on any failure.
//

var fs = require('node:fs');
var path = require('node:path');
var nurbs = require('../nurbs');
var createFairingPlan = require('../extras/fairing');

function fail (message) {
  process.stderr.write('fairing_obj: ' + message + '\n');
  process.exit(1);
}

var outPath = process.argv[2];
if (!outPath) fail('usage: node examples/fairing_obj.cjs <output.obj>');

// A non-planar, positively weighted rational surface with non-uniform knots.
var N = 5;
var points = [];
var weights = [];
for (var i = 0; i < N; i++) {
  points[i] = [];
  weights[i] = [];
  for (var j = 0; j < N; j++) {
    points[i][j] = [
      0.5 * i + 0.04 * i * i - 0.02 * i * j,
      0.46 * j + 0.03 * j * j + 0.015 * i * j,
      0.35 * Math.sin(0.62 * i + 0.45 * j) + 0.02 * i * j
    ];
    weights[i][j] = 1 + 0.16 * Math.sin(0.8 * i + 0.55 * j) + 0.015 * i * j;
  }
}
var knots = [
  [0, 0, 0, 0.85, 1.75, 2.6, 2.6, 2.6],
  [0, 0, 0, 0.7, 1.55, 2.4, 2.4, 2.4]
];
var surface = nurbs({
  points: points,
  weights: weights,
  degree: [2, 2],
  knots: knots,
  boundary: ['clamped', 'clamped']
});

var domain = surface.domain;
var anchors = [
  [domain[0][0] + 0.28 * (domain[0][1] - domain[0][0]), domain[1][0] + 0.34 * (domain[1][1] - domain[1][0])],
  [domain[0][0] + 0.62 * (domain[0][1] - domain[0][0]), domain[1][0] + 0.58 * (domain[1][1] - domain[1][0])],
  [domain[0][0] + 0.41 * (domain[0][1] - domain[0][0]), domain[1][0] + 0.77 * (domain[1][1] - domain[1][0])]
];

// Drag the sample points in the surface normal direction, so the surface
// really changes shape (it is non-planar to begin with).
var normalU = surface.evaluator([1, 0]);
var normalV = surface.evaluator([0, 1]);
var drag = [0.16, -0.12, 0.2];
var targets = anchors.map(function (anchor, k) {
  var p = surface.evaluate([], anchor[0], anchor[1]);
  var du = normalU([], anchor[0], anchor[1]);
  var dv = normalV([], anchor[0], anchor[1]);
  var n = [
    du[1] * dv[2] - du[2] * dv[1],
    du[2] * dv[0] - du[0] * dv[2],
    du[0] * dv[1] - du[1] * dv[0]
  ];
  var len = Math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (!(len > 1e-12)) fail('degenerate normal at anchor ' + k);
  var amount = drag[k];
  return [
    p[0] + amount * n[0] / len,
    p[1] + amount * n[1] / len,
    p[2] + amount * n[2] / len
  ];
});

var plan = createFairingPlan(surface, { anchors: anchors, lambda: 1e-3 });
var result = plan.solve(targets);
if (result.status !== 'ok' || !result.surface) {
  fail('solve failed with status ' + result.status);
}
if (!(result.constraintResidual <= 1e-8) || !(result.stationarityResidual <= 1e-8)) {
  fail('residuals too large: constraint=' + result.constraintResidual + ' stationarity=' + result.stationarityResidual);
}

var faired = result.surface;
var fairedU = faired.evaluator([1, 0]);
var fairedV = faired.evaluator([0, 1]);

var nu = 41;
var nv = 41;
var lines = [];
var u0 = domain[0][0];
var u1 = domain[0][1];
var v0 = domain[1][0];
var v1 = domain[1][1];

for (var a = 0; a < nu; a++) {
  var u = u0 + (u1 - u0) * a / (nu - 1);
  for (var b = 0; b < nv; b++) {
    var v = v0 + (v1 - v0) * b / (nv - 1);
    var p = faired.evaluate([], u, v);
    var du = fairedU([], u, v);
    var dv = fairedV([], u, v);
    var n = [
      du[1] * dv[2] - du[2] * dv[1],
      du[2] * dv[0] - du[0] * dv[2],
      du[0] * dv[1] - du[1] * dv[0]
    ];
    var len = Math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(len > 1e-12)) fail('degenerate normal at sample (' + a + ',' + b + ')');
    lines.push('v ' + p[0].toFixed(9) + ' ' + p[1].toFixed(9) + ' ' + p[2].toFixed(9));
    lines.push('vn ' + (n[0] / len).toFixed(9) + ' ' + (n[1] / len).toFixed(9) + ' ' + (n[2] / len).toFixed(9));
  }
}

for (a = 0; a + 1 < nu; a++) {
  for (var c = 0; c + 1 < nv; c++) {
    var i0 = a * nv + c + 1;
    var i1 = (a + 1) * nv + c + 1;
    var i2 = (a + 1) * nv + c + 2;
    var i3 = a * nv + c + 2;
    lines.push('f ' + i0 + '//' + i0 + ' ' + i1 + '//' + i1 + ' ' + i2 + '//' + i2);
    lines.push('f ' + i0 + '//' + i0 + ' ' + i2 + '//' + i2 + ' ' + i3 + '//' + i3);
  }
}

try {
  fs.mkdirSync(path.dirname(path.resolve(outPath)), { recursive: true });
  fs.writeFileSync(outPath, lines.join('\n') + '\n');
} catch (e) {
  fail('could not write ' + outPath + ': ' + (e && e.message));
}

process.stdout.write(
  'faired ' + anchors.length + ' anchors, energy=' + result.energy +
  ' constraintResidual=' + result.constraintResidual +
  ' stationarityResidual=' + result.stationarityResidual +
  ' vertices=' + (nu * nv) + ' triangles=' + (2 * (nu - 1) * (nv - 1)) + '\n'
);
