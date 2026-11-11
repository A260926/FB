'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const ndarray = require('ndarray');
const nurbs = require('../nurbs');

// ---------------------------------------------------------------------------
// Independent reference math (no library code involved)
// ---------------------------------------------------------------------------

// Column-major homogeneous matrix times point, with homogeneous division.
function projectPoint (m, p) {
  const n = p.length + 1;
  const h = p.concat([1]);
  const hp = new Array(n).fill(0);
  for (let r = 0; r < n; r++) {
    for (let c = 0; c < n; c++) hp[r] += m[c * n + r] * h[c];
  }
  return hp.slice(0, p.length).map((v) => v / hp[p.length]);
}

// Chain rule through the projection: derivative of pi(M * [p, 1]) where
// p moves with velocity v.
function projectDerivative (m, p, v) {
  const n = p.length + 1;
  const h = p.concat([1]);
  const hv = v.concat([0]);
  const num = new Array(n).fill(0);
  const dnum = new Array(n).fill(0);
  for (let r = 0; r < n; r++) {
    for (let c = 0; c < n; c++) {
      num[r] += m[c * n + r] * h[c];
      dnum[r] += m[c * n + r] * hv[c];
    }
  }
  const rho = num[p.length];
  const drho = dnum[p.length];
  return num.slice(0, p.length).map((nv, i) => (dnum[i] * rho - nv * drho) / (rho * rho));
}

// Column-major matrix product a * b.
function matMul (a, b, n) {
  const out = new Array(n * n).fill(0);
  for (let r = 0; r < n; r++) {
    for (let c = 0; c < n; c++) {
      for (let k = 0; k < n; k++) out[c * n + r] += a[k * n + r] * b[c * n + k];
    }
  }
  return out;
}

function relErr (actual, expected) {
  const num = Math.hypot(...actual.map((a, i) => a - expected[i]));
  const den = Math.max(1, Math.hypot(...expected));
  return num / den;
}

function assertClose (actual, expected, message) {
  assert.equal(actual.length, expected.length, message);
  const err = relErr(actual, expected);
  assert.ok(err < 1e-9, message + ' (relative error ' + err + ')');
}

// Genuinely projective 3x3 (column-major); last row is (0.3, 0.1, 1).
const M2D = [1, 0, 0.3, 0, 1, 0.1, 0.5, -0.2, 1];
// A second 3x3 that does not commute with M2D; last row is (0.2, 0.05, 1).
const M2D_B = [0, -1, 0.2, 1, 0, 0.05, 0.3, 0.4, 1];
// Affine 3x3 (last row (0, 0, 1)).
const A2D = [2, 0, 0, 0, 3, 0, 4, -2, 1];
// Genuinely projective 4x4; last row is (0.2, -0.1, 0.3, 1).
const M3D = [1, 0, 0, 0.2, 0, 1, 0, -0.1, 0, 0, 1, 0.3, 1, -2, 0.5, 1];

function makeRationalCurve () {
  return nurbs({
    points: [[0, 0], [1, 2], [3, 1], [4, 3], [5, 0]],
    weights: [1, 2, 0.5, 1.5, 1],
    degree: 3,
    knots: [0, 0, 0, 0, 1, 2, 2, 2, 2],
    boundary: 'clamped'
  });
}

function sampleCurve (spline, count) {
  const domain = spline.domain[0];
  const ts = [];
  for (let i = 0; i < count; i++) {
    ts.push(domain[0] + (domain[1] - domain[0]) * (i + 0.5) / count);
  }
  return ts;
}

// ---------------------------------------------------------------------------

test('projective rational curve matches pointwise projection and the chain rule', () => {
  const spline = makeRationalCurve();
  const ts = sampleCurve(spline, 9);
  const derivative = spline.evaluator(1);
  const originalPoints = ts.map((t) => spline.evaluate([], t));
  const originalDerivatives = ts.map((t) => derivative([], t));

  const degreeBefore = spline.degree.slice();
  const knotsBefore = spline.knots.map((k) => k.slice());
  const boundaryBefore = spline.boundary.slice();
  const domainBefore = spline.domain.map((d) => d.slice());
  const sizeBefore = spline.size.slice();

  const ret = spline.projectiveTransform(M2D);
  assert.equal(ret, spline, 'returns this');

  assert.deepEqual(spline.degree, degreeBefore);
  assert.deepEqual(spline.knots, knotsBefore);
  assert.deepEqual(spline.boundary, boundaryBefore);
  assert.deepEqual(spline.domain, domainBefore);
  assert.deepEqual(spline.size, sizeBefore);
  assert.ok(spline.weights.every((w) => w > 0 && Number.isFinite(w)), 'weights stay positive and finite');

  const derivativeAfter = spline.evaluator(1);
  ts.forEach((t, i) => {
    assertClose(spline.evaluate([], t), projectPoint(M2D, originalPoints[i]), 'point at t=' + t);
    assertClose(derivativeAfter([], t), projectDerivative(M2D, originalPoints[i], originalDerivatives[i]), 'derivative at t=' + t);
  });
});

test('projective promotes a non-rational curve to an owning rational representation', () => {
  const spline = nurbs({
    points: [[0, 0], [1, 2], [3, 1], [4, 3], [5, 0]],
    degree: 3,
    knots: [0, 0, 0, 0, 1, 2, 2, 2, 2],
    boundary: 'clamped'
  });
  assert.equal(spline.weights, undefined);

  const ts = sampleCurve(spline, 9);
  const derivative = spline.evaluator(1);
  const originalPoints = ts.map((t) => spline.evaluate([], t));
  const originalDerivatives = ts.map((t) => derivative([], t));

  spline.projectiveTransform(M2D);

  // Newly created weights: an owning nested array with the shape of the
  // control net (no coordinate component), not a view and not typed.
  assert.ok(Array.isArray(spline.weights));
  assert.equal(spline.weights.length, 5);
  assert.ok(spline.weights.every((w) => typeof w === 'number' && w > 0));
  assert.ok(!ArrayBuffer.isView(spline.weights));

  const derivativeAfter = spline.evaluator(1);
  ts.forEach((t, i) => {
    assertClose(spline.evaluate([], t), projectPoint(M2D, originalPoints[i]), 'point at t=' + t);
    assertClose(derivativeAfter([], t), projectDerivative(M2D, originalPoints[i], originalDerivatives[i]), 'derivative at t=' + t);
  });

  // Re-fetched rational basis and support use the new rational state.
  const t = ts[3];
  const basis = spline.evaluator(null, true);
  const basisDerivative = spline.evaluator(1, true);
  const support = spline.support([], t);
  assert.ok(support.length > 0);
  let basisSum = 0;
  let basisDerivativeSum = 0;
  const reconstructed = [0, 0];
  for (let i = 0; i < support.length; i++) {
    const b = basis(t, support[i]);
    basisSum += b;
    basisDerivativeSum += basisDerivative(t, support[i]);
    reconstructed[0] += b * spline.points[support[i]][0];
    reconstructed[1] += b * spline.points[support[i]][1];
  }
  assert.ok(Math.abs(basisSum - 1) < 1e-12, 'rational basis partitions unity');
  assert.ok(Math.abs(basisDerivativeSum) < 1e-9, 'rational basis derivatives sum to zero');
  assertClose(reconstructed, spline.evaluate([], t), 'basis reconstructs the evaluation');
});

test('projective transforms a 3d curve with a 4x4 matrix', () => {
  const spline = nurbs({
    points: [[0, 0, 0], [1, 2, 1], [3, 1, -1], [4, 0, 0.5]],
    weights: [1, 0.7, 1.3, 1],
    degree: 2,
    knots: [0, 0, 0, 1, 2, 2, 2],
    boundary: 'clamped'
  });

  const ts = sampleCurve(spline, 9);
  const derivative = spline.evaluator(1);
  const originalPoints = ts.map((t) => spline.evaluate([], t));
  const originalDerivatives = ts.map((t) => derivative([], t));

  spline.projectiveTransform(M3D);

  const derivativeAfter = spline.evaluator(1);
  ts.forEach((t, i) => {
    assertClose(spline.evaluate([], t), projectPoint(M3D, originalPoints[i]), 'point at t=' + t);
    assertClose(derivativeAfter([], t), projectDerivative(M3D, originalPoints[i], originalDerivatives[i]), 'derivative at t=' + t);
  });
});

test('projective non-planar surface matches projection and parametric partials', () => {
  const points = [];
  for (let i = 0; i < 4; i++) {
    const row = [];
    for (let j = 0; j < 4; j++) {
      row.push([i - 1.5, j - 1.5, Math.sin(i * 1.1) * Math.cos(j * 0.9)]);
    }
    points.push(row);
  }
  const spline = nurbs({ points: points, degree: 2, boundary: 'clamped' });

  const domain = spline.domain;
  const us = [];
  const vs = [];
  for (let i = 0; i < 5; i++) {
    us.push(domain[0][0] + (domain[0][1] - domain[0][0]) * (i + 0.5) / 5);
    vs.push(domain[1][0] + (domain[1][1] - domain[1][0]) * (i + 0.5) / 5);
  }
  const du = spline.evaluator([1, 0]);
  const dv = spline.evaluator([0, 1]);
  const samples = [];
  us.forEach((u) => vs.forEach((v) => {
    samples.push({
      u: u,
      v: v,
      p: spline.evaluate([], u, v),
      du: du([], u, v),
      dv: dv([], u, v)
    });
  }));
  const domainBefore = spline.domain.map((d) => d.slice());
  const sizeBefore = spline.size.slice();

  spline.projectiveTransform(M3D);

  assert.deepEqual(spline.domain, domainBefore);
  assert.deepEqual(spline.size, sizeBefore);
  assert.equal(spline.weights.length, 4);
  assert.equal(spline.weights[0].length, 4);

  const duAfter = spline.evaluator([1, 0]);
  const dvAfter = spline.evaluator([0, 1]);
  samples.forEach((s) => {
    assertClose(spline.evaluate([], s.u, s.v), projectPoint(M3D, s.p), 'surface point');
    assertClose(duAfter([], s.u, s.v), projectDerivative(M3D, s.p, s.du), 'du partial');
    assertClose(dvAfter([], s.u, s.v), projectDerivative(M3D, s.p, s.dv), 'dv partial');
  });
});

test('projective supports offset, transposed and negatively strided ndarray control nets', () => {
  // Curve: points in a negatively strided view, weights in a separate
  // negatively strided view, both surrounded by sentinels.
  const pointBuffer = new Float64Array(10);
  pointBuffer[0] = 111;
  pointBuffer[9] = -222;
  const control = [[0, 0], [1, 2], [3, 1], [4, 0]];
  for (let i = 0; i < 4; i++) {
    pointBuffer[7 - 2 * i] = control[i][0];
    pointBuffer[7 - 2 * i + 1] = control[i][1];
  }
  const points = ndarray(pointBuffer, [4, 2], [-2, 1], 7);

  const weightBuffer = new Float64Array(6);
  weightBuffer[0] = 333;
  weightBuffer[5] = -444;
  const controlWeights = [1, 2, 0.5, 1.5];
  for (let i = 0; i < 4; i++) weightBuffer[4 - i] = controlWeights[i];
  const weights = ndarray(weightBuffer, [4], [-1], 4);

  const spline = nurbs({
    points: points,
    weights: weights,
    degree: 2,
    knots: [0, 0, 0, 1, 2, 2, 2],
    boundary: 'clamped'
  });

  const ts = sampleCurve(spline, 7);
  const originalPoints = ts.map((t) => spline.evaluate([], t));

  spline.projectiveTransform(M2D);

  // Identity of the view objects and their underlying storage is preserved.
  assert.equal(spline.points, points);
  assert.equal(spline.weights, weights);
  assert.equal(spline.points.data, pointBuffer);
  assert.equal(spline.weights.data, weightBuffer);
  // Sentinels outside the views are untouched.
  assert.equal(pointBuffer[0], 111);
  assert.equal(pointBuffer[9], -222);
  assert.equal(weightBuffer[0], 333);
  assert.equal(weightBuffer[5], -444);

  // View contents match independently computed homogeneous transforms.
  for (let i = 0; i < 4; i++) {
    const h = [controlWeights[i] * control[i][0], controlWeights[i] * control[i][1], controlWeights[i]];
    const hp = [0, 0, 0];
    for (let r = 0; r < 3; r++) for (let c = 0; c < 3; c++) hp[r] += M2D[c * 3 + r] * h[c];
    assert.ok(Math.abs(pointBuffer[7 - 2 * i] - hp[0] / hp[2]) < 1e-12);
    assert.ok(Math.abs(pointBuffer[7 - 2 * i + 1] - hp[1] / hp[2]) < 1e-12);
    assert.ok(Math.abs(weightBuffer[4 - i] - hp[2]) < 1e-12);
  }

  ts.forEach((t, i) => {
    assertClose(spline.evaluate([], t), projectPoint(M2D, originalPoints[i]), 'point at t=' + t);
  });

  // Surface: transposed ndarray points with nested-array weights (mixed storage).
  const net = [
    [[0, 0, 0], [1, 0, 0.5], [2, 0, 0]],
    [[0, 1, 0.5], [1, 1, 1.5], [2, 1, 0.5]],
    [[0, 2, 0], [1, 2, 0.5], [2, 2, 0]]
  ];
  const netWeights = [[1, 1.2, 1], [0.8, 1.5, 0.9], [1, 1.1, 1]];
  const surfaceBuffer = new Float64Array(3 * 3 * 3);
  for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
      for (let d = 0; d < 3; d++) surfaceBuffer[d * 9 + i * 3 + j] = net[i][j][d];
    }
  }
  // Stored as [dimension, i, j], viewed transposed as [i, j, dimension].
  const surfacePoints = ndarray(surfaceBuffer, [3, 3, 3]).transpose(1, 2, 0);
  const surfaceWeights = netWeights.map((row) => row.slice());
  const surface = nurbs({
    points: surfacePoints,
    weights: surfaceWeights,
    degree: [2, 2],
    boundary: 'clamped'
  });

  const surfaceSamples = [];
  const sDomain = surface.domain;
  for (let a = 0; a < 3; a++) {
    for (let b = 0; b < 3; b++) {
      const u = sDomain[0][0] + (sDomain[0][1] - sDomain[0][0]) * (a + 0.5) / 3;
      const v = sDomain[1][0] + (sDomain[1][1] - sDomain[1][0]) * (b + 0.5) / 3;
      surfaceSamples.push({ u: u, v: v, p: surface.evaluate([], u, v) });
    }
  }

  surface.projectiveTransform(M3D);

  assert.equal(surface.points, surfacePoints);
  assert.equal(surface.points.data, surfaceBuffer);
  assert.equal(surface.weights, surfaceWeights);
  surfaceSamples.forEach((s) => {
    assertClose(surface.evaluate([], s.u, s.v), projectPoint(M3D, s.p), 'transposed surface point');
  });
});

test('projective preserves the seam of a closed curve', () => {
  const spline = nurbs({
    points: [[1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0], [-1, -1], [0, -1], [1, -1]],
    weights: [1, 0.8, 1.2, 0.9, 1, 1.1, 0.7, 1.3],
    degree: 2,
    boundary: 'closed'
  });
  const size = spline.size[0];

  const seamBefore = spline.evaluate([], 0);
  const wrapBefore = spline.evaluate([], size);
  assertClose(wrapBefore, seamBefore, 'closed seam before transform');

  const ts = sampleCurve(spline, 11);
  const originalPoints = ts.map((t) => spline.evaluate([], t));

  spline.projectiveTransform(M2D);

  const seamAfter = spline.evaluate([], 0);
  assertClose(spline.evaluate([], size), seamAfter, 'closed seam after transform');
  assertClose(seamAfter, projectPoint(M2D, seamBefore), 'seam point matches projection');
  ts.forEach((t, i) => {
    assertClose(spline.evaluate([], t), projectPoint(M2D, originalPoints[i]), 'point at t=' + t);
  });
});

test('projective is invariant to overall matrix scale', () => {
  const a = makeRationalCurve();
  const b = makeRationalCurve();
  const scale = -2.5;
  const scaled = M2D.map((x) => x * scale);

  a.projectiveTransform(M2D);
  b.projectiveTransform(scaled);

  // The scaled matrix flips every denominator negative; weights must
  // still come out positive.
  assert.ok(b.weights.every((w) => w > 0 && Number.isFinite(w)));

  sampleCurve(a, 9).forEach((t) => {
    assertClose(b.evaluate([], t), a.evaluate([], t), 'scale-invariant point at t=' + t);
  });
});

test('projective composes non-commuting transforms like matrix products', () => {
  assert.ok(M2D.some((x, i) => Math.abs(x - M2D_B[i]) > 1e-12));
  const composed = matMul(M2D_B, M2D, 3);
  const swapped = matMul(M2D, M2D_B, 3);
  assert.ok(composed.some((x, i) => Math.abs(x - swapped[i]) > 1e-6), 'matrices do not commute');

  const sequential = makeRationalCurve();
  sequential.projectiveTransform(M2D);
  sequential.projectiveTransform(M2D_B);

  const singleShot = makeRationalCurve();
  singleShot.projectiveTransform(composed);

  sampleCurve(sequential, 9).forEach((t) => {
    assertClose(sequential.evaluate([], t), singleShot.evaluate([], t), 'composed point at t=' + t);
  });
});

test('projective interleaves with affine transform and isolates sibling caches', () => {
  const interleaved = makeRationalCurve();
  const singleShot = makeRationalCurve();
  const bystander = makeRationalCurve();

  const ts = sampleCurve(bystander, 7);
  const bystanderBefore = ts.map((t) => bystander.evaluate([], t));

  interleaved.projectiveTransform(M2D);
  interleaved.transform(A2D);
  interleaved.projectiveTransform(M2D_B);

  singleShot.projectiveTransform(matMul(M2D_B, matMul(A2D, M2D, 3), 3));

  ts.forEach((t, i) => {
    assertClose(interleaved.evaluate([], t), singleShot.evaluate([], t), 'interleaved point at t=' + t);
    // A structurally identical object created from the same inputs shares
    // generated code but must not observe the transformed data.
    assertClose(bystander.evaluate([], t), bystanderBefore[i], 'bystander point at t=' + t);
  });

  // A sibling created after the transform evaluates its own data.
  const lateSibling = makeRationalCurve();
  ts.forEach((t, i) => {
    assertClose(lateSibling.evaluate([], t), bystanderBefore[i], 'late sibling point at t=' + t);
  });
});

test('projective rejects invalid input without mutating state', () => {
  function assertRejected (setup, m, pattern) {
    const spline = setup();
    const pointsBefore = JSON.parse(JSON.stringify(spline.points));
    const weightsBefore = spline.weights ? JSON.parse(JSON.stringify(spline.weights)) : undefined;
    const ts = sampleCurve(spline, 5);
    const evaluatedBefore = ts.map((t) => spline.evaluate([], t));

    assert.throws(() => spline.projectiveTransform(m), pattern);

    assert.deepEqual(JSON.parse(JSON.stringify(spline.points)), pointsBefore, 'points unchanged');
    assert.equal(spline.weights === undefined, weightsBefore === undefined, 'rational state unchanged');
    if (weightsBefore !== undefined) {
      assert.deepEqual(JSON.parse(JSON.stringify(spline.weights)), weightsBefore, 'weights unchanged');
    }
    ts.forEach((t, i) => {
      // deepEqual treats NaN as equal to NaN, which matters for the cases
      // with non-finite inputs; the evaluation must be bit-for-bit unchanged.
      assert.deepEqual(spline.evaluate([], t), evaluatedBefore[i], 'evaluation still works at t=' + t);
    });
  }

  // Zero homogeneous denominator: 1 - x / 3 vanishes at the control point x = 3.
  assertRejected(makeRationalCurve, [1, 0, -1 / 3, 0, 1, 0, 0, 0, 1], RangeError);
  // Mixed denominator signs: positive at x = 0, negative at x = 5.
  assertRejected(makeRationalCurve, [1, 0, -1, 0, 1, 0, 0, 0, 1.5], RangeError);
  // Non-finite matrix entries.
  assertRejected(makeRationalCurve, [1, 0, NaN, 0, 1, 0, 0, 0, 1], RangeError);
  assertRejected(makeRationalCurve, [1, 0, 0, 0, 1, 0, 0, 0, Infinity], RangeError);
  // Wrong matrix size.
  assertRejected(makeRationalCurve, [1, 0, 0, 1], RangeError);
  // Non-positive or non-finite pre-existing weights.
  assertRejected(() => nurbs({
    points: [[0, 0], [1, 2], [3, 0]],
    weights: [1, -2, 1],
    degree: 2,
    knots: [0, 0, 0, 1, 1, 1],
    boundary: 'clamped'
  }), M2D, RangeError);
  assertRejected(() => nurbs({
    points: [[0, 0], [1, 2], [3, 0]],
    weights: [1, NaN, 1],
    degree: 2,
    knots: [0, 0, 0, 1, 1, 1],
    boundary: 'clamped'
  }), M2D, RangeError);
  // Non-finite control points.
  assertRejected(() => nurbs({
    points: [[0, 0], [1, Infinity], [3, 0]],
    weights: [1, 2, 1],
    degree: 2,
    knots: [0, 0, 0, 1, 1, 1],
    boundary: 'clamped'
  }), M2D, RangeError);
});

test('projective does not support generic ndarray storage', () => {
  const store = { 0: 0, 1: 0, 2: 1, 3: 2, 4: 3, 5: 0 };
  const points = ndarray({
    get: (i) => store[i],
    set: (i, v) => { store[i] = v; }
  }, [3, 2]);
  assert.equal(points.dtype, 'generic');
  const spline = nurbs({
    points: points,
    degree: 2,
    knots: [0, 0, 0, 1, 1, 1],
    boundary: 'clamped'
  });
  assert.throws(() => spline.projectiveTransform(M2D), /generic ndarray/);
});
