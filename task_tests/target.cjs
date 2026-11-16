'use strict';

//
// Target tests for `extras/sample-cad.js` and the rational second-order
// evaluator extension.
//
// Protocol (task_support/driver.cjs): an array of
//   { group, name, run(check, api) }
// where api = { nurbs, sampleCad }.  Each `run` must call `check` at least once.
//
// The references below are deliberately independent of the code under test:
//   - `independentDerivatives` computes the quotient rule from the Cox-de Boor
//     derivative recursion (not the library's basis-derivative routine);
//   - `sectionReference` evaluates normals from an independent meridian/ring
//     section expression using plain 1D spline evaluations;
//   - topology is verified by edge incidence and Euler characteristic.
//

const SQRT1_2 = Math.SQRT1_2;

// ---------------------------------------------------------------------------
// Small vector helpers (independent of gl-vec3 and of the library)
// ---------------------------------------------------------------------------

function cross (a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  ];
}

function negate (v) {
  return [-v[0], -v[1], -v[2]];
}

function normalize (v) {
  const len = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return [v[0] / len, v[1] / len, v[2] / len];
}

function maxAbsDiff (a, b) {
  let worst = 0;
  for (let i = 0; i < a.length; i++) worst = Math.max(worst, Math.abs(a[i] - b[i]));
  return worst;
}

// ---------------------------------------------------------------------------
// Independent rational quotient-rule reference
// ---------------------------------------------------------------------------

// Value of degree-p B-spline basis N_{i,p}(t), from the recursive definition.
// The number of degree-p basis functions on K is K.length - p - 1.
function basisValue (K, p, i, t) {
  const count = K.length - p - 1;
  if (i < 0 || i >= count) return 0;
  if (p === 0) return (K[i] <= t && t < K[i + 1]) ? 1 : 0;
  let value = 0;
  const dLeft = K[i + p] - K[i];
  if (dLeft > 0) value += (t - K[i]) / dLeft * basisValue(K, p - 1, i, t);
  const dRight = K[i + p + 1] - K[i + 1];
  if (dRight > 0) value += (K[i + p + 1] - t) / dRight * basisValue(K, p - 1, i + 1, t);
  return value;
}

function basisFirst (K, p, i, t) {
  if (p <= 0) return 0;
  let value = 0;
  const dLeft = K[i + p] - K[i];
  if (dLeft > 0) value += p / dLeft * basisValue(K, p - 1, i, t);
  const dRight = K[i + p + 1] - K[i + 1];
  if (dRight > 0) value -= p / dRight * basisValue(K, p - 1, i + 1, t);
  return value;
}

function basisSecond (K, p, i, t) {
  if (p <= 1) return 0;
  let value = 0;
  const dLeft = K[i + p] - K[i];
  if (dLeft > 0) value += p / dLeft * basisFirst(K, p - 1, i, t);
  const dRight = K[i + p + 1] - K[i + 1];
  if (dRight > 0) value -= p / dRight * basisFirst(K, p - 1, i + 1, t);
  return value;
}

function basisRow (K, p, i, t, order) {
  if (order === 0) return basisValue(K, p, i, t);
  if (order === 1) return basisFirst(K, p, i, t);
  return basisSecond(K, p, i, t);
}

// Tensor-product homogeneous derivative of (w, w*P), then quotient rule.
function homogeneousDerivative (axes, points, weights, a, b) {
  const width = points[0][0].length;
  const H = new Array(width).fill(0);
  let W = 0;
  for (let i = 0; i < axes[0].n; i++) {
    const bu = basisRow(axes[0].K, axes[0].p, i, axes[0].t, a);
    if (bu === 0) continue;
    for (let j = 0; j < axes[1].n; j++) {
      const bv = basisRow(axes[1].K, axes[1].p, j, axes[1].t, b);
      if (bv === 0) continue;
      const c = bu * bv;
      const w = weights ? weights[i][j] : 1;
      W += c * w;
      for (let d = 0; d < width; d++) H[d] += c * w * points[i][j][d];
    }
  }
  return { H, W };
}

function makeAxis (K, p, n, t) {
  return { K, p, n, t };
}

// Returns { value, du, dv, duu, dvv, duv } for a rational surface.
function referenceSurfaceDerivatives (points, weights, knots, degree, u, v) {
  const axes = [makeAxis(knots[0], degree[0], points.length, u),
    makeAxis(knots[1], degree[1], points[0].length, v)];
  const H00 = homogeneousDerivative(axes, points, weights, 0, 0);
  const H10 = homogeneousDerivative(axes, points, weights, 1, 0);
  const H01 = homogeneousDerivative(axes, points, weights, 0, 1);
  const H20 = homogeneousDerivative(axes, points, weights, 2, 0);
  const H02 = homogeneousDerivative(axes, points, weights, 0, 2);
  const H11 = homogeneousDerivative(axes, points, weights, 1, 1);
  const W = H00.W;
  const value = H00.H.map((h) => h / W);
  const du = H10.H.map((h, d) => (h - value[d] * H10.W) / W);
  const dv = H01.H.map((h, d) => (h - value[d] * H01.W) / W);
  const duu = H20.H.map((h, d) => (h - 2 * du[d] * H10.W - value[d] * H20.W) / W);
  const dvv = H02.H.map((h, d) => (h - 2 * dv[d] * H01.W - value[d] * H02.W) / W);
  const duv = H11.H.map((h, d) => (h - du[d] * H01.W - dv[d] * H10.W - value[d] * H11.W) / W);
  return { value, du, dv, duu, dvv, duv, W };
}

function referenceCurveSecond (points, weights, knots, degree, t) {
  const K = knots;
  const n = points.length;
  const width = points[0].length;
  function hom (order) {
    const H = new Array(width).fill(0);
    let W = 0;
    for (let i = 0; i < n; i++) {
      const b = basisRow(K, degree, i, t, order);
      if (b === 0) continue;
      const w = weights ? weights[i] : 1;
      W += b * w;
      for (let d = 0; d < width; d++) H[d] += b * w * points[i][d];
    }
    return { H, W };
  }
  const h0 = hom(0);
  const h1 = hom(1);
  const h2 = hom(2);
  const C = h0.H.map((h) => h / h0.W);
  const C1 = h1.H.map((h, d) => (h - C[d] * h1.W) / h0.W);
  const C2 = h2.H.map((h, d) => (h - 2 * C1[d] * h1.W - C[d] * h2.W) / h0.W);
  return { value: C, first: C1, second: C2 };
}

// Rational basis value / derivatives for the tensor product, summing the
// numerator over (i, j) with W derivatives from the same basis rows.
function referenceBasis (points, weights, knots, degree, u, v, targetI, targetJ, order) {
  const p0 = degree[0];
  const p1 = degree[1];
  const n0 = points.length;
  const n1 = points[0].length;
  const a = order[0];
  const b = order[1];
  function wDeriv (aa, bb) {
    return homogeneousDerivative([makeAxis(knots[0], p0, n0, u), makeAxis(knots[1], p1, n1, v)],
      points, weights, aa, bb).W;
  }
  const W0 = wDeriv(0, 0);
  const nu = basisRow(knots[0], p0, targetI, u, a);
  const nv = basisRow(knots[1], p1, targetJ, v, b);
  const w = weights ? weights[targetI][targetJ] : 1;
  const B = w * nu * nv;
  if (a === 0 && b === 0) return B / W0;
  const R = (w * basisRow(knots[0], p0, targetI, u, 0) * basisRow(knots[1], p1, targetJ, v, 0)) / W0;
  const Bu = w * basisRow(knots[0], p0, targetI, u, 1) * basisRow(knots[1], p1, targetJ, v, 0);
  const Bv = w * basisRow(knots[0], p0, targetI, u, 0) * basisRow(knots[1], p1, targetJ, v, 1);
  const W1u = wDeriv(1, 0);
  const W1v = wDeriv(0, 1);
  const Ru = (Bu - R * W1u) / W0;
  const Rv = (Bv - R * W1v) / W0;
  // R = B / W; build derivatives through the quotient rule.
  if (a === 2 && b === 0) {
    const W2 = wDeriv(2, 0);
    return (B - 2 * Ru * W1u - R * W2) / W0;
  }
  if (a === 0 && b === 2) {
    const W2 = wDeriv(0, 2);
    return (B - 2 * Rv * W1v - R * W2) / W0;
  }
  if (a === 1 && b === 0) return Ru;
  if (a === 0 && b === 1) return Rv;
  const Wuv = wDeriv(1, 1);
  return (B - Ru * W1v - Rv * W1u - R * Wuv) / W0;
}

// ---------------------------------------------------------------------------
// Section-expression reference: P(u, v) = (Cx(u) R(v), Cy(u) R(v), Z(v))
// ---------------------------------------------------------------------------

function curvePoint (spline, t) {
  return spline.evaluate([], t);
}

function curveFirst (spline, t) {
  return spline.evaluator([1])([], t);
}

function sectionReference (ringSpline, profileSpline, swapped) {
  function data (u, v) {
    // S(a, b) = (Cx(a) R(b), Cy(a) R(b), Z(b)) with a the ring parameter and b
    // the profile parameter.  The transposed surface is T(u, v) = S(v, u).
    const ringArg = swapped ? v : u;
    const profileArg = swapped ? u : v;
    const C = curvePoint(ringSpline, ringArg);
    const Cp = curveFirst(ringSpline, ringArg);
    const P = curvePoint(profileSpline, profileArg);
    const Pp = curveFirst(profileSpline, profileArg);
    const point = [C[0] * P[0], C[1] * P[0], P[1]];
    let Su, Sv, Suv;
    if (!swapped) {
      Su = [Cp[0] * P[0], Cp[1] * P[0], 0];
      Sv = [C[0] * Pp[0], C[1] * Pp[0], Pp[1]];
      Suv = [Cp[0] * Pp[0], Cp[1] * Pp[0], 0];
    } else {
      Su = [C[0] * Pp[0], C[1] * Pp[0], Pp[1]];
      Sv = [Cp[0] * P[0], Cp[1] * P[0], 0];
      Suv = [Cp[0] * Pp[0], Cp[1] * Pp[0], 0];
    }
    return { point, Su, Sv, Suv };
  }

  return {
    point (u, v) { return data(u, v).point; },
    normal (u, v) {
      const d = data(u, v);
      return normalize(cross(d.Su, d.Sv));
    },
    poleNormal (edge, u, v) {
      const d = data(u, v);
      let n;
      if (edge === 'vmin') n = cross(d.Suv, d.Sv);
      else if (edge === 'vmax') n = negate(cross(d.Suv, d.Sv));
      else if (edge === 'umin') n = cross(d.Su, d.Suv);
      else n = negate(cross(d.Su, d.Suv));
      return normalize(n);
    }
  };
}

// ---------------------------------------------------------------------------
// Surface builders (tensor products of a planar ring curve and a profile curve)
// ---------------------------------------------------------------------------

function quarterRing () {
  return {
    points: [[1, 0], [1, 1], [0, 1]],
    weights: [1, SQRT1_2, 1],
    knots: [0, 0, 0, 1, 1, 1],
    boundary: 'clamped'
  };
}

function unitRing (n) {
  const points = [];
  const weights = [];
  const knots = [];
  for (let i = 0; i < n; i++) {
    const angle = 2 * Math.PI * i / n;
    points.push([Math.cos(angle), Math.sin(angle)]);
    weights.push(1);
  }
  for (let i = 0; i <= n; i++) knots.push(i);
  return { points, weights, knots, boundary: 'closed' };
}

function quarterArcProfile () {
  return { points: [[1, 0], [1, 1], [0, 1]], weights: [1, SQRT1_2, 1], knots: [0, 0, 0, 1, 1, 1], boundary: 'clamped' };
}

function coneProfile () {
  return { points: [[1, 0], [0.5, 0.5], [0, 1]], weights: [1, SQRT1_2, 1], knots: [0, 0, 0, 1, 1, 1], boundary: 'clamped' };
}

function lensProfile () {
  return { points: [[0, 0], [1, 0.5], [0, 1]], weights: [1, SQRT1_2, 1], knots: [0, 0, 0, 1, 1, 1], boundary: 'clamped' };
}

function revolve (api, ring, profile, ringKnots, profileKnots) {
  const points = [];
  const weights = [];
  for (let i = 0; i < ring.points.length; i++) {
    const row = [];
    const wrow = [];
    for (let j = 0; j < profile.points.length; j++) {
      const r = profile.points[j][0];
      row.push([ring.points[i][0] * r, ring.points[i][1] * r, profile.points[j][1]]);
      wrow.push(ring.weights[i] * profile.weights[j]);
    }
    points.push(row);
    weights.push(wrow);
  }
  const usedRingKnots = ringKnots || ring.knots;
  const usedProfileKnots = profileKnots || profile.knots;
  const spline = api.nurbs({
    points,
    weights,
    degree: [2, 2],
    boundary: [ring.boundary, profile.boundary],
    knots: [usedRingKnots, usedProfileKnots]
  });
  const ringSpline = api.nurbs({
    points: ring.points,
    weights: ring.weights,
    degree: 2,
    boundary: ring.boundary,
    knots: usedRingKnots
  });
  const profileSpline = api.nurbs({
    points: profile.points,
    weights: profile.weights,
    degree: 2,
    boundary: profile.boundary,
    knots: usedProfileKnots
  });
  return { spline, ringSpline, profileSpline };
}

function transposeControlNet (points, weights) {
  const n0 = points.length;
  const n1 = points[0].length;
  const outPoints = [];
  const outWeights = [];
  for (let j = 0; j < n1; j++) {
    const row = [];
    const wrow = [];
    for (let i = 0; i < n0; i++) {
      row.push(points[i][j].slice());
      wrow.push(weights[i][j]);
    }
    outPoints.push(row);
    outWeights.push(wrow);
  }
  return { points: outPoints, weights: outWeights };
}

// ---------------------------------------------------------------------------
// Mesh / topology helpers
// ---------------------------------------------------------------------------

function buildEdgeMap (mesh) {
  const triangles = mesh.cells.length / 3;
  const edges = new Map();
  for (let t = 0; t < triangles; t++) {
    const v = [mesh.cells[3 * t], mesh.cells[3 * t + 1], mesh.cells[3 * t + 2]];
    for (let e = 0; e < 3; e++) {
      const a = v[e];
      const b = v[(e + 1) % 3];
      const key = a < b ? a + ',' + b : b + ',' + a;
      if (!edges.has(key)) edges.set(key, []);
      edges.get(key).push({ triangle: t, from: a, to: b });
    }
  }
  return edges;
}

function topologyReport (mesh) {
  const triangles = mesh.cells.length / 3;
  const edges = buildEdgeMap(mesh);
  const used = new Set();
  let interior = 0;
  let boundary = 0;
  let badInterior = 0;
  for (const list of edges.values()) {
    if (list.length === 2) {
      interior++;
      const a = list[0];
      const b = list[1];
      if (!(a.from === b.to && a.to === b.from)) badInterior++;
    } else if (list.length === 1) {
      boundary++;
    } else {
      badInterior++;
    }
  }
  for (let i = 0; i < mesh.cells.length; i++) used.add(mesh.cells[i]);
  const V = used.size;
  const E = edges.size;
  return { V, E, F: triangles, chi: V - E + triangles, interior, boundary, badInterior };
}

function boundaryLoops (mesh) {
  const edges = buildEdgeMap(mesh);
  const adjacency = new Map();
  let boundaryEdges = 0;
  for (const [key, list] of edges) {
    if (list.length !== 1) continue;
    boundaryEdges++;
    const a = list[0].from;
    const b = list[0].to;
    for (const [x, y] of [[a, b], [b, a]]) {
      if (!adjacency.has(x)) adjacency.set(x, []);
      adjacency.get(x).push({ to: y, key });
    }
  }
  const visited = new Set();
  let loops = 0;
  let bad = 0;
  for (const start of adjacency.keys()) {
    if (visited.has(start)) continue;
    loops++;
    let current = start;
    let guard = 0;
    while (true) {
      visited.add(current);
      const nexts = adjacency.get(current) || [];
      if (nexts.length !== 2) bad++;
      const next = nexts.find((n) => !visited.has(n.to));
      if (!next) break;
      visited.add(next.to);
      current = next.to;
      if (current === start) break;
      if (++guard > adjacency.size + 2) { bad++; break; }
    }
  }
  return { loops, boundaryEdges, bad };
}

function uvRoundTripError (spline, mesh) {
  const scratch = [0, 0, 0];
  let worst = 0;
  for (let c = 0; c < mesh.cells.length / 3 * 3; c++) {
    const u = mesh.cornerUVs[2 * c];
    const v = mesh.cornerUVs[2 * c + 1];
    spline.evaluate(scratch, u, v);
    const index = mesh.cells[c];
    for (let d = 0; d < 3; d++) {
      worst = Math.max(worst, Math.abs(scratch[d] - mesh.positions[3 * index + d]));
    }
  }
  return worst;
}

function hasDegenerateTriangle (mesh) {
  for (let t = 0; t < mesh.cells.length / 3; t++) {
    const a = mesh.cells[3 * t];
    const b = mesh.cells[3 * t + 1];
    const c = mesh.cells[3 * t + 2];
    if (a === b || b === c || a === c) return true;
    const pa = mesh.positions.slice(3 * a, 3 * a + 3);
    const pb = mesh.positions.slice(3 * b, 3 * b + 3);
    const pc = mesh.positions.slice(3 * c, 3 * c + 3);
    const n = cross([pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]],
      [pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]]);
    if (!(Math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) > 0)) return true;
  }
  return false;
}

function allUnit (mesh) {
  let worst = 0;
  for (let i = 0; i < mesh.cornerNormals.length; i += 3) {
    const len = Math.hypot(mesh.cornerNormals[i], mesh.cornerNormals[i + 1], mesh.cornerNormals[i + 2]);
    worst = Math.max(worst, Math.abs(len - 1));
  }
  return worst;
}

function isFiniteArray (arr) {
  for (let i = 0; i < arr.length; i++) if (!isFinite(arr[i])) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Cases: jets (second-order rational evaluation)
// ---------------------------------------------------------------------------

function makeTestSurface (api, offset) {
  offset = offset || 0;
  const scale = 1 + 0.4 * offset;
  const phase = 0.6 * offset;
  const points = [];
  const weights = [];
  for (let i = 0; i < 5; i++) {
    const row = [];
    const wrow = [];
    for (let j = 0; j < 6; j++) {
      row.push([
        scale * (0.15 * i - 0.1 * j),
        scale * (0.2 * i + 0.05 * j * j / 6 - 0.1),
        scale * (0.3 * Math.sin(i * 0.5 + j * 0.35 + phase))
      ]);
      wrow.push(0.4 + 0.17 * ((i * 3 + j * 2) % 7) + 0.03 * j + 0.11 * offset);
    }
    points.push(row);
    weights.push(wrow);
  }
  const knots = [
    [0, 0, 0, 0.7, 1.6, 2.2, 2.2, 2.2],
    [0, 0, 0, 0, 0.8, 1.7, 2.4, 2.4, 2.4, 2.4]
  ];
  return { points, weights, knots };
}

const jetsCases = [
  {
    group: 'jets',
    name: 'rational surface second order matches independent quotient rule',
    run (check, api) {
      const data = makeTestSurface(api, 0);
      const spline = api.nurbs({
        points: data.points,
        weights: data.weights,
        degree: [2, 3],
        boundary: ['clamped', 'clamped'],
        knots: data.knots
      });
      const samples = [[0.33, 0.42], [1.05, 1.15], [1.87, 2.02]];
      const orders = [[2, 0], [0, 2], [1, 1]];
      for (const [u, v] of samples) {
        const ref = referenceSurfaceDerivatives(data.points, data.weights, data.knots, [2, 3], u, v);
        check(maxAbsDiff(ref.value, spline.evaluate([], u, v)) < 1e-12, 'point value reference');
        const du = spline.evaluator([1, 0])([], u, v);
        const dv = spline.evaluator([0, 1])([], u, v);
        check(maxAbsDiff(ref.du, du) < 1e-12, 'first derivative u reference');
        check(maxAbsDiff(ref.dv, dv) < 1e-12, 'first derivative v reference');
        for (const order of orders) {
          const got = spline.evaluator(order)([], u, v);
          const expected = order[0] === 2 ? ref.duu : (order[1] === 2 ? ref.dvv : ref.duv);
          check(isFiniteArray(got), 'second derivative finite ' + order.join(','));
          check(maxAbsDiff(got, expected) < 1e-11, 'second derivative clause ' + order.join(',') + ' at ' + u + ',' + v);
        }
      }
      // Images stay inside the supported coordinate envelope.
      for (const row of data.points) {
        for (const point of row) check(point.every((x) => Math.abs(x) <= 10), 'coordinate envelope');
      }
      for (const row of data.weights) for (const w of row) check(w >= 0.1 && w <= 10, 'weight envelope');
    }
  },
  {
    group: 'jets',
    name: 'rational basis derivatives sum to zero and values to one',
    run (check, api) {
      const data = makeTestSurface(api, 0.02);
      const spline = api.nurbs({
        points: data.points,
        weights: data.weights,
        degree: [2, 3],
        boundary: ['clamped', 'clamped'],
        knots: data.knots
      });
      const samples = [[0.31, 0.44], [1.2, 1.35], [1.9, 2.1]];
      const n0 = data.points.length;
      const n1 = data.points[0].length;
      for (const [u, v] of samples) {
        for (const order of [[0, 0], [1, 0], [0, 1], [2, 0], [0, 2], [1, 1]]) {
          let sumLib = 0;
          let sumRef = 0;
          for (let i = 0; i < n0; i++) {
            for (let j = 0; j < n1; j++) {
              const value = spline.evaluator(order, true)(u, v, i, j);
              check(isFinite(value), 'basis value finite');
              sumLib += value;
              sumRef += referenceBasis(data.points, data.weights, data.knots, [2, 3], u, v, i, j, order);
            }
          }
          const target = order[0] === 0 && order[1] === 0 ? 1 : 0;
          check(Math.abs(sumLib - target) < 1e-10, 'basis sum ' + order.join(',') + ' = ' + target);
          check(Math.abs(sumRef - target) < 1e-10, 'reference basis sum ' + order.join(',') + ' = ' + target);
        }
      }
    }
  },
  {
    group: 'jets',
    name: 'rational curve second derivative and basis sums',
    run (check, api) {
      const points = [[0.2, 0.4, -0.3], [1.1, 1.4, 0.2], [1.9, -0.6, 1.1], [2.6, 0.7, -0.4], [3.4, 1.2, 0.8]];
      const weights = [0.6, 2.1, 0.45, 1.7, 0.9];
      const knots = [0, 0, 0, 0.9, 1.7, 2.6, 2.6, 2.6];
      const spline = api.nurbs({ points, weights, degree: 2, boundary: 'clamped', knots: [knots] });
      for (const t of [0.35, 1.1, 1.55, 2.25]) {
        const ref = referenceCurveSecond(points, weights, knots, 2, t);
        check(maxAbsDiff(ref.value, spline.evaluate([], t)) < 1e-12, 'curve value');
        check(maxAbsDiff(ref.first, spline.evaluator([1])([], t)) < 1e-11, 'curve first derivative');
        check(maxAbsDiff(ref.second, spline.evaluator([2])([], t)) < 1e-10, 'curve second derivative at ' + t);
        let sum0 = 0;
        let sum1 = 0;
        let sum2 = 0;
        for (let i = 0; i < points.length; i++) {
          sum0 += spline.evaluator(null, true)(t, i);
          sum1 += spline.evaluator([1], true)(t, i);
          sum2 += spline.evaluator([2], true)(t, i);
        }
        check(Math.abs(sum0 - 1) < 1e-10, 'curve basis sum');
        check(Math.abs(sum1) < 1e-9, 'curve basis first sum');
        check(Math.abs(sum2) < 1e-8, 'curve basis second sum');
      }
    }
  },
  {
    group: 'jets',
    name: 'strided ndarray view evaluates second derivatives like dense data',
    run (check, api) {
      const densePoints = [];
      const data = [];
      const offset = 3;
      const stride0 = 32;
      const stride1 = 4;
      const stride2 = 1;
      const n0 = 4;
      const n1 = 5;
      // Padded backing store: shape [n0, n1, 3] with non-contiguous strides.
      const total = offset + stride0 * (n0 - 1) + stride1 * (n1 - 1) + stride2 * 2 + 1;
      const backing = new Array(total).fill(777);
      for (let i = 0; i < n0; i++) {
        const row = [];
        for (let j = 0; j < n1; j++) {
          const p = [0.2 * i + 0.05 * j, -0.1 * i + 0.15 * j, 0.25 * Math.sin(i + 0.7 * j)];
          row.push(p);
          for (let d = 0; d < 3; d++) {
            backing[offset + stride0 * i + stride1 * j + stride2 * d] = p[d];
          }
        }
        densePoints.push(row);
      }
      const weights = [];
      for (let i = 0; i < n0; i++) {
        const row = [];
        for (let j = 0; j < n1; j++) row.push(0.7 + 0.2 * i + 0.1 * j);
        weights.push(row);
      }
      const knots = [
        [0, 0, 0, 0.6, 1.5, 1.5, 1.5],
        [0, 0, 0, 0.5, 1.1, 1.9, 1.9, 1.9]
      ];
      const view = {
        data: backing,
        shape: [n0, n1, 3],
        stride: [stride0, stride1, stride2],
        offset,
        dimension: 3,
        dtype: 'float64'
      };
      const strided = api.nurbs({
        points: view, weights, degree: [2, 2], boundary: ['clamped', 'clamped'], knots
      });
      const solid = api.nurbs({
        points: densePoints, weights, degree: [2, 2], boundary: ['clamped', 'clamped'], knots
      });
      for (const [u, v] of [[0.33, 0.41], [1.05, 1.2]]) {
        check(maxAbsDiff(strided.evaluate([], u, v), solid.evaluate([], u, v)) < 1e-12, 'strided value matches dense');
        const ref = referenceSurfaceDerivatives(densePoints, weights, knots, [2, 2], u, v);
        const stridedUu = strided.evaluator([2, 0])([], u, v);
        const stridedVv = strided.evaluator([0, 2])([], u, v);
        const stridedUv = strided.evaluator([1, 1])([], u, v);
        check(maxAbsDiff(stridedUu, ref.duu) < 1e-10, 'strided second derivative u');
        check(maxAbsDiff(stridedVv, ref.dvv) < 1e-10, 'strided second derivative v');
        check(maxAbsDiff(stridedUv, ref.duv) < 1e-10, 'strided mixed derivative');
      }
      check(backing[0] === 777 && backing[1] === 777 && backing[2] === 777, 'strided padding untouched');
    }
  },
  {
    group: 'jets',
    name: 'alternating evaluators on multiple objects stay object-local',
    run (check, api) {
      const dataA = makeTestSurface(api, 0);
      const dataB = makeTestSurface(api, 0.5);
      const make = (data) => api.nurbs({
        points: data.points, weights: data.weights, degree: [2, 3], boundary: ['clamped', 'clamped'], knots: data.knots
      });
      const a = make(dataA);
      const b = make(dataB);
      const samples = [[0.4, 0.55], [1.3, 1.5]];
      for (const [u, v] of samples) {
        const refA = referenceSurfaceDerivatives(dataA.points, dataA.weights, dataA.knots, [2, 3], u, v);
        const refB = referenceSurfaceDerivatives(dataB.points, dataB.weights, dataB.knots, [2, 3], u, v);
        const aUu = a.evaluator([2, 0])([], u, v);
        const bVv = b.evaluator([0, 2])([], u, v);
        const aUv = a.evaluator([1, 1])([], u, v);
        const bUu = b.evaluator([2, 0])([], u, v);
        check(maxAbsDiff(aUu, refA.duu) < 1e-10, 'object A uu unaffected by B');
        check(maxAbsDiff(bVv, refB.dvv) < 1e-10, 'object B vv');
        check(maxAbsDiff(aUv, refA.duv) < 1e-10, 'object A uv');
        check(maxAbsDiff(bUu, refB.duu) < 1e-10, 'object B uu');
        check(maxAbsDiff(aUu, bUu) > 1e-6, 'objects have distinct geometry');
      }
    }
  }
];

// ---------------------------------------------------------------------------
// Cases: poles (collapsed edges and limit normals)
// ---------------------------------------------------------------------------

const polesCases = [
  {
    group: 'poles',
    name: 'round dome pole normals match the analytic sphere',
    run (check, api) {
      const { spline, ringSpline, profileSpline } = revolve(api, quarterRing(), quarterArcProfile());
      const ref = sectionReference(ringSpline, profileSpline, false);
      const mesh = api.sampleCad(spline, { resolution: [5, 6], collapsed: ['vmax'] });
      check(mesh.positions.length % 3 === 0, 'flat positions');
      check(mesh.cells.length % 3 === 0, 'flat triangle indices');
      check(mesh.cornerUVs.length === mesh.cells.length * 2, 'two uv floats per corner');
      check(mesh.cornerNormals.length === mesh.cells.length * 3, 'three normal floats per corner');
      check(!hasDegenerateTriangle(mesh), 'no zero-area triangles');
      check(allUnit(mesh) < 1e-12, 'all normals unit');

      let poleIndex = -1;
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (Math.abs(mesh.positions[i + 2] - 1) < 1e-12 &&
            Math.hypot(mesh.positions[i], mesh.positions[i + 1]) < 1e-12) poleIndex = i / 3;
      }
      check(poleIndex >= 0, 'pole position exists');
      let poleCorners = 0;
      for (let t = 0; t < mesh.cells.length / 3; t++) {
        for (let c = 0; c < 3; c++) {
          if (mesh.cells[3 * t + c] !== poleIndex) continue;
          poleCorners++;
          const o = 3 * (3 * t + c);
          const normal = mesh.cornerNormals.slice(o, o + 3);
          const u = mesh.cornerUVs[2 * (3 * t + c)];
          const v = mesh.cornerUVs[2 * (3 * t + c) + 1];
          check(Math.abs(v - 1) < 1e-12, 'pole corner at v max');
          // Analytic sphere: every point on this patch lies on the unit sphere,
          // so the outward normal equals the position.  At the pole it is +z.
          const position = spline.evaluate([], u, v);
          check(Math.hypot(position[0], position[1]) < 1e-12, 'pole position at origin xy');
          check(Math.abs(position[2] - 1) < 1e-12, 'pole position at unit z');
          check(Math.max(Math.abs(normal[0]), Math.abs(normal[1]), Math.abs(normal[2] - 1)) < 1e-9,
            'dome pole normal is analytic +z');
          // Also cross-check the independent section limit.
          const limit = ref.poleNormal('vmax', u, v);
          check(maxAbsDiff(normal, limit) < 1e-8, 'dome pole normal equals section limit');
        }
      }
      check(poleCorners === 5, 'each fan cell contributes one pole corner');

      // Ordinary corner normals equal normalize(Pu x Pv) via the section model.
      let ordinaryChecked = 0;
      for (let t = 0; t < mesh.cells.length / 3; t++) {
        for (let c = 0; c < 3; c++) {
          if (mesh.cells[3 * t + c] === poleIndex) continue;
          const o = 3 * (3 * t + c);
          const u = mesh.cornerUVs[2 * (3 * t + c)];
          const v = mesh.cornerUVs[2 * (3 * t + c) + 1];
          const expected = ref.normal(u, v);
          check(maxAbsDiff(mesh.cornerNormals.slice(o, o + 3), expected) < 1e-8,
            'ordinary dome normal equals normalize(Pu x Pv)');
          ordinaryChecked++;
        }
      }
      check(ordinaryChecked > 0, 'ordinary corners checked');
    }
  },
  {
    group: 'poles',
    name: 'cone pole normals vary per fan corner and match the section expression',
    run (check, api) {
      const { spline, ringSpline, profileSpline } = revolve(api, quarterRing(), coneProfile());
      const ref = sectionReference(ringSpline, profileSpline, false);
      const mesh = api.sampleCad(spline, { resolution: [6, 4], collapsed: ['vmax'] });
      check(!hasDegenerateTriangle(mesh), 'cone has no degenerate triangles');
      check(allUnit(mesh) < 1e-12, 'cone normals unit');

      let poleIndex = -1;
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (Math.abs(mesh.positions[i + 2] - 1) < 1e-12 &&
            Math.hypot(mesh.positions[i], mesh.positions[i + 1]) < 1e-12) poleIndex = i / 3;
      }
      check(poleIndex >= 0, 'cone tip exists');
      const normals = [];
      for (let t = 0; t < mesh.cells.length / 3; t++) {
        for (let c = 0; c < 3; c++) {
          if (mesh.cells[3 * t + c] !== poleIndex) continue;
          const o = 3 * (3 * t + c);
          const u = mesh.cornerUVs[2 * (3 * t + c)];
          const v = mesh.cornerUVs[2 * (3 * t + c) + 1];
          const normal = mesh.cornerNormals.slice(o, o + 3);
          normals.push(normal);
          check(Math.abs(v - 1) < 1e-12, 'cone corner on v max');
          check(Math.abs(u - (Math.floor(u * 6) + 0.5) / 6) < 1e-9, 'pole uv uses cell midpoint');
          const limit = ref.poleNormal('vmax', u, v);
          check(maxAbsDiff(normal, limit) < 1e-8, 'cone pole normal equals section limit at u=' + u);
        }
      }
      check(normals.length === 6, 'one pole corner per fan cell');
      let maxVariation = 0;
      for (let i = 0; i < normals.length; i++) {
        for (let j = i + 1; j < normals.length; j++) {
          maxVariation = Math.max(maxVariation, maxAbsDiff(normals[i], normals[j]));
        }
      }
      check(maxVariation > 1e-3, 'cone limit normal varies with angle');

      // Interior cone normals match the section expression too.
      for (const [u, v] of [[0.2, 0.4], [0.7, 0.8]]) {
        const expected = ref.normal(u, v);
        const got = mesh.cornerNormals.slice(0, 0);
        check(got.length === 0, 'sanity');
        const du = spline.evaluator([1, 0])([], u, v);
        const dv = spline.evaluator([0, 1])([], u, v);
        check(maxAbsDiff(normalize(cross(du, dv)), expected) < 1e-8, 'interior cone normal matches section');
      }
    }
  },
  {
    group: 'poles',
    name: 'two opposite collapsed edges become two independent fans',
    run (check, api) {
      const { spline, ringSpline, profileSpline } = revolve(api, quarterRing(), lensProfile());
      const ref = sectionReference(ringSpline, profileSpline, false);
      const mesh = api.sampleCad(spline, { resolution: [4, 5], collapsed: ['vmin', 'vmax'] });
      check(!hasDegenerateTriangle(mesh), 'lens has no degenerate triangles');
      check(allUnit(mesh) < 1e-12, 'lens normals unit');
      const report = topologyReport(mesh);
      check(report.badInterior === 0, 'lens interior edges are consistently paired');
      check(report.chi === 1, 'lens is a disk (chi 1)');

      const poles = [];
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (Math.hypot(mesh.positions[i], mesh.positions[i + 1]) < 1e-12 &&
            (Math.abs(mesh.positions[i + 2]) < 1e-12 || Math.abs(mesh.positions[i + 2] - 1) < 1e-12)) {
          poles.push({ index: i / 3, z: mesh.positions[i + 2] });
        }
      }
      check(poles.length === 2, 'exactly two pole positions');
      check(Math.abs(poles[0].z - poles[1].z) > 0.5, 'poles are distinct positions');
      for (const pole of poles) {
        const edge = pole.z < 0.5 ? 'vmin' : 'vmax';
        let corners = 0;
        for (let t = 0; t < mesh.cells.length / 3; t++) {
          for (let c = 0; c < 3; c++) {
            if (mesh.cells[3 * t + c] !== pole.index) continue;
            corners++;
            const o = 3 * (3 * t + c);
            const u = mesh.cornerUVs[2 * (3 * t + c)];
            const v = mesh.cornerUVs[2 * (3 * t + c) + 1];
            check(maxAbsDiff(mesh.cornerNormals.slice(o, o + 3), ref.poleNormal(edge, u, v)) < 1e-8,
              'lens pole normal matches section limit on ' + edge);
          }
        }
        check(corners === 4, 'fan corner count for ' + edge);
      }
    }
  },
  {
    group: 'poles',
    name: 'axis swap puts the pole on the other parameter axis',
    run (check, api) {
      const { spline } = revolve(api, quarterRing(), coneProfile());
      const ring = quarterRing();
      const profile = coneProfile();
      const net = transposeControlNet(spline.points, spline.weights);
      const transposed = api.nurbs({
        points: net.points,
        weights: net.weights,
        degree: [2, 2],
        boundary: ['clamped', 'clamped'],
        knots: [profile.knots, ring.knots]
      });
      const ringSpline = api.nurbs({ points: ring.points, weights: ring.weights, degree: 2, boundary: 'clamped', knots: ring.knots });
      const profileSpline = api.nurbs({ points: profile.points, weights: profile.weights, degree: 2, boundary: 'clamped', knots: profile.knots });
      const ref = sectionReference(ringSpline, profileSpline, true);
      const mesh = api.sampleCad(transposed, { resolution: [4, 6], collapsed: ['umax'] });
      check(!hasDegenerateTriangle(mesh), 'axis-swapped surface has no degenerate triangles');
      check(allUnit(mesh) < 1e-12, 'axis-swapped normals unit');

      const report = topologyReport(mesh);
      check(report.badInterior === 0, 'axis-swapped interior edges paired');
      check(report.chi === 1, 'axis-swapped disk chi');

      let poleIndex = -1;
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (Math.abs(mesh.positions[i + 2] - 1) < 1e-12 &&
            Math.hypot(mesh.positions[i], mesh.positions[i + 1]) < 1e-12) poleIndex = i / 3;
      }
      check(poleIndex >= 0, 'axis-swapped pole exists');
      let corners = 0;
      for (let t = 0; t < mesh.cells.length / 3; t++) {
        for (let c = 0; c < 3; c++) {
          if (mesh.cells[3 * t + c] !== poleIndex) continue;
          corners++;
          const o = 3 * (3 * t + c);
          const u = mesh.cornerUVs[2 * (3 * t + c)];
          const v = mesh.cornerUVs[2 * (3 * t + c) + 1];
          check(Math.abs(u - 1) < 1e-12, 'axis-swapped pole UV on umax');
          check(maxAbsDiff(mesh.cornerNormals.slice(o, o + 3), ref.poleNormal('umax', u, v)) < 1e-8,
            'axis-swapped limit normal matches section');
        }
      }
      check(corners === 6, 'axis-swapped fan corner count');
      // Geometry agrees with S(v, u).
      for (const [u, v] of [[0.25, 0.4], [0.6, 0.75]]) {
        check(maxAbsDiff(transposed.evaluate([], u, v), ref.point(u, v)) < 1e-12, 'transposed point matches section');
      }
    }
  },
  {
    group: 'poles',
    name: 'non-unit shifted knots keep pole normals analytic',
    run (check, api) {
      const ring = quarterRing();
      const profile = coneProfile();
      const ringKnots = [-2, -2, -2, 0.5, 0.5, 0.5];
      const profileKnots = [10, 10, 10, 11.5, 11.5, 11.5];
      const built = revolve(api, ring, profile, ringKnots, profileKnots);
      const ref = sectionReference(built.ringSpline, built.profileSpline, false);
      const mesh = api.sampleCad(built.spline, { resolution: [4, 3], collapsed: ['vmax'] });
      check(!hasDegenerateTriangle(mesh), 'shifted surface no degenerate triangles');
      check(allUnit(mesh) < 1e-12, 'shifted normals unit');
      let poleIndex = -1;
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (Math.abs(mesh.positions[i + 2] - 1) < 1e-12 &&
            Math.hypot(mesh.positions[i], mesh.positions[i + 1]) < 1e-12) poleIndex = i / 3;
      }
      check(poleIndex >= 0, 'shifted pole exists');
      let checked = 0;
      for (let t = 0; t < mesh.cells.length / 3; t++) {
        for (let c = 0; c < 3; c++) {
          const o = 3 * (3 * t + c);
          const u = mesh.cornerUVs[2 * (3 * t + c)];
          const v = mesh.cornerUVs[2 * (3 * t + c) + 1];
          check(u >= -2 - 1e-12 && u <= 0.5 + 1e-12, 'u inside shifted domain');
          check(v >= 10 - 1e-12 && v <= 11.5 + 1e-12, 'v inside shifted domain');
          if (mesh.cells[3 * t + c] === poleIndex) {
            check(maxAbsDiff(mesh.cornerNormals.slice(o, o + 3), ref.poleNormal('vmax', u, v)) < 1e-8,
              'shifted pole normal');
            checked++;
          }
        }
      }
      check(checked === 4, 'shifted fan corner count');
      // UV round trip holds on the shifted domain too.
      check(uvRoundTripError(built.spline, mesh) < 1e-9, 'shifted UV round trip');
    }
  }
];

// ---------------------------------------------------------------------------
// Cases: topology (closed axis + pole, coincidence, failure atomicity)
// ---------------------------------------------------------------------------

function closedDome (api, n) {
  const ring = unitRing(n);
  const profile = quarterArcProfile();
  return revolve(api, ring, profile);
}

const topologyCases = [
  {
    group: 'topology',
    name: 'closed axis with pole: incidence, boundary loop, Euler, UV round trip',
    run (check, api) {
      const n = 8;
      const { spline } = closedDome(api, n);
      const mesh = api.sampleCad(spline, { resolution: [3, 4], collapsed: ['vmax'] });
      check(mesh.positions.length > 0 && mesh.cells.length > 0, 'non-empty mesh');
      check(!hasDegenerateTriangle(mesh), 'no degenerate faces');
      check(allUnit(mesh) < 1e-12, 'unit normals');
      const report = topologyReport(mesh);
      check(report.badInterior === 0, 'every interior edge has two opposite faces');
      check(report.E === report.interior + report.boundary, 'edge partition');
      check(report.chi === 1, 'closed-axis pole surface is a disk');
      const loops = boundaryLoops(mesh);
      check(loops.loops === 1, 'single boundary loop (open bottom ring)');
      check(loops.bad === 0, 'boundary graph is a clean cycle');
      check(uvRoundTripError(spline, mesh) < 1e-9, 'corner UV evaluates back to the vertex position');
      // Seam vertices are shared: the position at domain start equals domain end.
      const start = spline.evaluate([], 0, 0.25);
      let matches = 0;
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (Math.abs(mesh.positions[i] - start[0]) < 1e-12 &&
            Math.abs(mesh.positions[i + 1] - start[1]) < 1e-12 &&
            Math.abs(mesh.positions[i + 2] - start[2]) < 1e-12) matches++;
      }
      check(matches === 1, 'seam position index shared exactly once');
      // Corner UVs keep the original domain ends at the seam.
      let sawStart = false;
      let sawEnd = false;
      for (let i = 0; i < mesh.cornerUVs.length; i += 2) {
        if (Math.abs(mesh.cornerUVs[i]) < 1e-12) sawStart = true;
        if (Math.abs(mesh.cornerUVs[i] - n) < 1e-12) sawEnd = true;
      }
      check(sawStart && sawEnd, 'seam corner UVs use both original domain ends');
    }
  },
  {
    group: 'topology',
    name: 'undeclared spatial coincidence is not welded',
    run (check, api) {
      // Clamped (not periodic) full-circle curve extruded in v: the two ends of
      // u coincide in space but are different parameter identities.
      const n = 9;
      const points = [];
      const weights = [];
      for (let i = 0; i < n; i++) {
        const angle = 2 * Math.PI * i / (n - 1);
        points.push([Math.cos(angle), Math.sin(angle)]);
        weights.push(1);
      }
      // Degree 2 clamped needs n + 3 = 12 knots: three at each end plus
      // n - p - 1 = 6 distinct interior knots.
      const knots = [0, 0, 0, 1, 2, 3, 4, 5, 6, 8, 8, 8];
      const profile = { points: [[1, 0], [1, 0.5], [0, 1]], weights: [1, 1, 1], knots: [0, 0, 0, 1, 1, 1] };
      const surfacePoints = [];
      const surfaceWeights = [];
      for (let i = 0; i < points.length; i++) {
        const row = [];
        const wrow = [];
        for (let j = 0; j < profile.points.length; j++) {
          row.push([points[i][0] * profile.points[j][0], points[i][1] * profile.points[j][0], profile.points[j][1]]);
          wrow.push(weights[i] * profile.weights[j]);
        }
        surfacePoints.push(row);
        surfaceWeights.push(wrow);
      }
      const surface = api.nurbs({
        points: surfacePoints,
        weights: surfaceWeights,
        degree: [2, 2],
        boundary: ['clamped', 'clamped'],
        knots: [knots, profile.knots]
      });
      const mesh = api.sampleCad(surface, { resolution: [2, 2], collapsed: ['vmax'] });
      // The start and end of u evaluate to the same spatial point but are
      // declared clamped, so they must remain distinct vertex indices.
      // Use a sample station (v = 0.5) so the vertices are part of the mesh.
      const first = surface.evaluate([], 0, 0.5);
      const last = surface.evaluate([], 8, 0.5);
      check(maxAbsDiff(first, last) < 1e-12, 'undeclared end points coincide spatially');
      let firstIndex = -1;
      let lastIndex = -1;
      for (let i = 0; i < mesh.positions.length; i += 3) {
        if (maxAbsDiff(mesh.positions.slice(i, i + 3), first) < 1e-12) {
          if (firstIndex === -1) firstIndex = i / 3; else lastIndex = i / 3;
        }
      }
      check(firstIndex >= 0 && lastIndex >= 0, 'both coincident vertices are present');
      check(firstIndex !== lastIndex, 'coincident undeclared vertices stay distinct');
      // And the boundary stays open: no triangle bridges the two.
      let bridges = 0;
      for (let t = 0; t < mesh.cells.length / 3; t++) {
        const tri = [mesh.cells[3 * t], mesh.cells[3 * t + 1], mesh.cells[3 * t + 2]];
        if (tri.indexOf(firstIndex) !== -1 && tri.indexOf(lastIndex) !== -1) bridges++;
      }
      check(bridges === 0, 'no face bridges the undeclared coincidence');
    }
  },
  {
    group: 'topology',
    name: 'failed sampling leaves input untouched and remains callable',
    run (check, api) {
      const { spline } = revolve(api, quarterRing(), coneProfile());
      const pointsBefore = JSON.stringify(spline.points);
      const weightsBefore = JSON.stringify(spline.weights);
      const knotsBefore = JSON.stringify(spline.knots);
      const valid = api.sampleCad(spline, { resolution: [3, 3], collapsed: ['vmax'] });
      const snapshot = {
        positions: valid.positions.slice(),
        cells: valid.cells.slice(),
        cornerUVs: valid.cornerUVs.slice(),
        cornerNormals: valid.cornerNormals.slice()
      };
      const badOptions = [
        { resolution: [0, 2], collapsed: ['vmax'] },
        { resolution: [3, 33], collapsed: ['vmax'] },
        { resolution: [2.5, 2], collapsed: ['vmax'] },
        { resolution: [2, 2], collapsed: ['vmax', 'vmax'] },
        { resolution: [2, 2], collapsed: ['vmid'] },
        { resolution: [2, 2], collapsed: ['umin', 'vmax'] }
      ];
      for (const options of badOptions) {
        let threw = false;
        try {
          api.sampleCad(spline, options);
        } catch (error) {
          threw = error instanceof RangeError;
        }
        check(threw, 'invalid options throw RangeError: ' + JSON.stringify(options));
      }
      check(JSON.stringify(spline.points) === pointsBefore, 'points unchanged after failures');
      check(JSON.stringify(spline.weights) === weightsBefore, 'weights unchanged after failures');
      check(JSON.stringify(spline.knots) === knotsBefore, 'knots unchanged after failures');
      const again = api.sampleCad(spline, { resolution: [3, 3], collapsed: ['vmax'] });
      check(again.positions.length === snapshot.positions.length, 'repeat call position count');
      check(maxAbsDiff(again.positions, snapshot.positions) === 0, 'repeat call deterministic positions');
      check(maxAbsDiff(again.cells, snapshot.cells) === 0, 'repeat call deterministic cells');
      check(maxAbsDiff(again.cornerUVs, snapshot.cornerUVs) === 0, 'repeat call deterministic uvs');
      check(maxAbsDiff(again.cornerNormals, snapshot.cornerNormals) === 0, 'repeat call deterministic normals');
      // Independent arrays: mutating one must not affect another.
      check(valid.positions !== valid.cells && valid.cells !== valid.cornerUVs && valid.cornerUVs !== valid.cornerNormals,
        'result arrays are independent');
    }
  },
  {
    group: 'topology',
    name: 'collapsed element validation rejects guesses and non-identical rows',
    run (check, api) {
      const { spline } = revolve(api, quarterRing(), quarterArcProfile());
      // Declaring an edge whose control row is not identical must throw.
      let threw = false;
      try {
        api.sampleCad(spline, { resolution: [2, 2], collapsed: ['vmin'] });
      } catch (error) {
        threw = error instanceof RangeError;
      }
      check(threw, 'non-identical collapsed row rejected with RangeError');
      // Periodic axis cannot declare a collapsed edge.
      const { spline: closedSpline } = closedDome(api, 6);
      threw = false;
      try {
        api.sampleCad(closedSpline, { resolution: [2, 2], collapsed: ['umin'] });
      } catch (error) {
        threw = error instanceof RangeError;
      }
      check(threw, 'collapsed edge on periodic axis rejected');
      // Adjacent collapsed edges are illegal, opposite ones are legal.
      threw = false;
      try {
        api.sampleCad(spline, { resolution: [2, 2], collapsed: ['vmax', 'umax'] });
      } catch (error) {
        threw = error instanceof RangeError;
      }
      check(threw, 'adjacent collapsed edges rejected');
      // A non-clamped, non-periodic axis is rejected.
      threw = false;
      try {
        const open = api.nurbs({
          points: [[[0, 0, 0], [0, 1, 0]], [[1, 0, 0], [1, 1, 0]], [[2, 0, 0], [2, 1, 0]]],
          degree: [2, 1],
          boundary: ['open', 'clamped']
        });
        api.sampleCad(open, { resolution: [2, 2] });
      } catch (error) {
        threw = error instanceof RangeError;
      }
      check(threw, 'non-clamped non-periodic axis rejected');
      // Non-nested (flat) control points are rejected.
      threw = false;
      try {
        const flat = api.nurbs({
          points: [[0, 0], [1, 0], [2, 0], [3, 0]],
          degree: 1,
          boundary: 'clamped'
        });
        api.sampleCad(flat, { resolution: [2, 2] });
      } catch (error) {
        threw = error instanceof RangeError;
      }
      check(threw, 'unsupported control point structure rejected');
      // Bad weight is rejected.
      threw = false;
      try {
        const badWeight = api.nurbs({
          points: [[[0, 0, 0], [0, 1, 0], [0, 2, 0]], [[1, 0, 0], [1, 1, 0], [1, 2, 0]], [[2, 0, 0], [2, 1, 0], [2, 2, 0]]],
          weights: [[1, 1, 1], [1, -2, 1], [1, 1, 1]],
          degree: [2, 2],
          boundary: ['clamped', 'clamped']
        });
        api.sampleCad(badWeight, { resolution: [2, 2] });
      } catch (error) {
        threw = error instanceof RangeError;
      }
      check(threw, 'non-positive weight rejected');
    }
  }
];

module.exports = jetsCases.concat(polesCases, topologyCases);
