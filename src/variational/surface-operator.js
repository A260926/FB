'use strict';

//
// Reference metric and discrete energy operator for the surface fairing
// problem. Everything here is assembled once, at construction time, from a
// deep copy of the reference surface: solve() then only reuses the fixed
// matrices against a right hand side coming from the constraints.
//
// The displacement field D(u, v) uses the same rational basis as the source
// surface, with control point displacements as coefficients. With the
// reference first and second fundamental quantities
//
//   g_ab = Sa . Sb
//   Gamma^c_ab = sum_d g^cd Sd . Sab
//   H_ab(D) = Dab - sum_c Gamma^c_ab Dc
//
// (each ambient coordinate treated independently), the discrete energy is
//
//   E = sum_q w_q sqrt(det g) [ sum_{a,b,c,d} g^ac g^bd H_ab.H_cd
//                               + lambda D.D ]
//
// evaluated with a 6x6 Gauss-Legendre product rule on every non-empty knot
// rectangle. Since D = sum_j B_j d_j this is a quadratic form
//
//   E = 1/2 sum_ij d_i^T K_ij d_j
//
// in the control point displacements d_i; K is what solve() minimizes
// subject to the point constraints.
//

// 6 point Gauss-Legendre rule on [-1, 1], used as a 6x6 product rule.
var GL_X = [
  -0.9324695142031521,
  -0.6612093864662645,
  -0.2386191860831969,
  0.2386191860831969,
  0.6612093864662645,
  0.9324695142031521
];
var GL_W = [
  0.17132449237917036,
  0.3607615730481386,
  0.46791393457269104,
  0.46791393457269104,
  0.3607615730481386,
  0.17132449237917036
];

// Smallest admissible sine of the angle between the two tangents at an
// integration point. The reference surface must be regular.
var MIN_TANGENT_SINE = 1e-6;

function fail (message) {
  throw new RangeError('createFairingPlan: ' + message);
}

function isFiniteNumber (x) {
  return typeof x === 'number' && isFinite(x);
}

//
// Deep copy of the reference data we depend on. Plain arrays, typed arrays
// and ndarray style containers are copied so later edits of the source cannot
// reach the plan.
//
function cloneData (value) {
  var i, out, keys;
  if (Array.isArray(value)) {
    out = new Array(value.length);
    for (i = 0; i < value.length; i++) out[i] = cloneData(value[i]);
    return out;
  }
  if (value && typeof value === 'object') {
    if (typeof ArrayBuffer !== 'undefined' && ArrayBuffer.isView(value)) {
      return new value.constructor(value);
    }
    if (typeof value.data !== 'undefined' && typeof value.shape !== 'undefined') {
      out = {};
      keys = Object.keys(value);
      for (i = 0; i < keys.length; i++) {
        if (keys[i] === 'data') out.data = cloneData(value.data);
        else out[keys[i]] = cloneData(value[keys[i]]);
      }
      return out;
    }
    out = {};
    keys = Object.keys(value);
    for (i = 0; i < keys.length; i++) out[keys[i]] = cloneData(value[keys[i]]);
    return out;
  }
  return value;
}

//
// Validate the restricted input domain accepted by this entry point. Returns
// the parameter domain.
//
function validateSurface (spline) {
  var d, i, k, u;

  if (!spline || (typeof spline !== 'object' && typeof spline !== 'function')) fail('a nurbs surface is required');
  if (spline.splineDimension !== 2) fail('expected a two parameter surface');
  if (spline.dimension !== 3) fail('expected three dimensional control points');
  if (!Array.isArray(spline.points) || !spline.points.length) {
    fail('nested array control points are required');
  }

  var size = spline.size;
  if (!size || size.length !== 2) fail('invalid control hull size');
  if (size[0] * size[1] > 128) fail('at most 128 control points are supported');
  if (size[0] < 2 || size[1] < 2) fail('at least two control points per axis are required');

  var degree = spline.degree;
  if (!degree || degree.length !== 2) fail('invalid degree');
  for (d = 0; d < 2; d++) {
    if (degree[d] !== 2 && degree[d] !== 3) {
      fail('only polynomial degree 2 or 3 is supported, got ' + degree[d] + ' in dimension ' + (d + 1));
    }
    if (size[d] <= degree[d]) {
      fail('expected at least ' + (degree[d] + 1) + ' control points in dimension ' + (d + 1));
    }
  }

  var boundary = spline.boundary;
  if (!boundary || boundary.length !== 2) fail('invalid boundary modes');
  for (d = 0; d < 2; d++) {
    if (boundary[d] !== 'clamped') {
      fail('only clamped open surfaces are supported, got "' + boundary[d] + '" in dimension ' + (d + 1));
    }
  }

  var knots = spline.knots;
  if (!Array.isArray(knots) || knots.length !== 2 || !Array.isArray(knots[0]) || !Array.isArray(knots[1])) {
    fail('explicit knot vectors are required');
  }
  for (d = 0; d < 2; d++) {
    var kv = knots[d];
    var n = size[d];
    var p = degree[d];
    if (kv.length !== n + p + 1) fail('expected ' + (n + p + 1) + ' knots in dimension ' + (d + 1) + ' but got ' + kv.length);
    for (i = 0; i < kv.length; i++) {
      if (!isFiniteNumber(kv[i])) fail('non-finite knot in dimension ' + (d + 1));
      if (Math.abs(kv[i]) > 10) fail('knot value out of range in dimension ' + (d + 1));
      if (i > 0) {
        if (!(kv[i] >= kv[i - 1])) fail('knots must be non-decreasing in dimension ' + (d + 1));
        if (kv[i] > kv[i - 1] && kv[i] - kv[i - 1] < 1e-3) fail('knot spacing below 1e-3 in dimension ' + (d + 1));
      }
    }
    for (i = 0; i <= p; i++) {
      if (kv[i] !== kv[0]) fail('clamped knots must repeat the first knot ' + (p + 1) + ' times in dimension ' + (d + 1));
      if (kv[kv.length - 1 - i] !== kv[kv.length - 1]) fail('clamped knots must repeat the last knot ' + (p + 1) + ' times in dimension ' + (d + 1));
    }
    for (i = p + 1; i < n; i++) {
      if (!(kv[i] > kv[i - 1])) fail('interior knots must be simple in dimension ' + (d + 1));
    }
  }

  var weights = spline.weights;
  if (!Array.isArray(weights) || weights.length !== size[0]) fail('nested array weights are required');
  for (i = 0; i < size[0]; i++) {
    if (!Array.isArray(weights[i]) || weights[i].length !== size[1]) fail('invalid weight grid');
    for (k = 0; k < size[1]; k++) {
      var w = weights[i][k];
      if (!isFiniteNumber(w)) fail('non-finite weight');
      if (!(w >= 0.1) || w > 10) fail('weights must lie in [0.1, 10]');
    }
  }

  for (i = 0; i < size[0]; i++) {
    if (!Array.isArray(spline.points[i]) || spline.points[i].length !== size[1]) fail('invalid control point grid');
    for (k = 0; k < size[1]; k++) {
      var pt = spline.points[i][k];
      if (!Array.isArray(pt) || pt.length !== 3) fail('control points must be three dimensional nested arrays');
      for (d = 0; d < 3; d++) {
        if (!isFiniteNumber(pt[d])) fail('non-finite control point coordinate');
        if (Math.abs(pt[d]) > 10) fail('control point coordinate out of range');
      }
    }
  }

  var domain = [
    [knots[0][degree[0]], knots[0][size[0]]],
    [knots[1][degree[1]], knots[1][size[1]]]
  ];
  for (d = 0; d < 2; d++) {
    if (!(domain[d][1] > domain[d][0])) fail('degenerate parameter domain in dimension ' + (d + 1));
    for (u = 0; u < 2; u++) {
      if (Math.abs(domain[d][u]) > 10) fail('parameter value out of range');
    }
  }

  return domain;
}

//
// Gauss-Legendre product rule over the non-empty knot rectangles. Each
// rectangle contributes 6x6 nodes; the weights include the mapping Jacobian.
//
function buildQuadrature (domain, knots, size, degree) {
  var points = [];
  var breaks = [[], []];
  var d, i, j, a, b;

  for (d = 0; d < 2; d++) {
    breaks[d].push(domain[d][0]);
    for (i = degree[d] + 1; i < size[d]; i++) {
      if (knots[d][i] > domain[d][0] && knots[d][i] < domain[d][1]) breaks[d].push(knots[d][i]);
    }
    breaks[d].push(domain[d][1]);
  }

  var us = breaks[0];
  var vs = breaks[1];
  for (i = 0; i + 1 < us.length; i++) {
    var u0 = us[i];
    var u1 = us[i + 1];
    if (!(u1 > u0)) continue;
    var uh = 0.5 * (u1 - u0);
    var um = 0.5 * (u1 + u0);
    for (j = 0; j + 1 < vs.length; j++) {
      var v0 = vs[j];
      var v1 = vs[j + 1];
      if (!(v1 > v0)) continue;
      var vh = 0.5 * (v1 - v0);
      var vm = 0.5 * (v1 + v0);
      for (a = 0; a < 6; a++) {
        for (b = 0; b < 6; b++) {
          points.push({
            u: um + uh * GL_X[a],
            v: vm + vh * GL_X[b],
            weight: GL_W[a] * GL_W[b] * uh * vh
          });
        }
      }
    }
  }

  if (!points.length) fail('empty quadrature');
  return points;
}

function spanIndex (u, knots, size, degree) {
  var low = degree;
  var high = size - 1;
  var mid;
  if (u >= knots[high]) return high;
  if (u <= knots[low]) return low;
  while (high - low > 1) {
    mid = (low + high) >> 1;
    if (u < knots[mid]) high = mid; else low = mid;
  }
  return low;
}

//
// Univariate B-spline basis, first and second derivatives at `u`, restricted
// to the p + 1 active functions of the containing span. Entries are ordered
// by local index 0..p, corresponding to control points span - p .. span.
// This is the standard derivative triangle (Piegl & Tiller A2.3).
//
function basisDerivatives (u, knots, size, degree) {
  var p = degree;
  var span = spanIndex(u, knots, size, degree);
  var i, j, r, k;

  var ndu = [];
  for (i = 0; i <= p; i++) {
    ndu[i] = new Array(p + 1);
    for (j = 0; j <= p; j++) ndu[i][j] = 0;
  }
  var left = new Array(p + 1);
  var right = new Array(p + 1);
  ndu[0][0] = 1;
  for (j = 1; j <= p; j++) {
    left[j] = u - knots[span + 1 - j];
    right[j] = knots[span + j] - u;
    var saved = 0;
    for (r = 0; r < j; r++) {
      ndu[j][r] = right[r + 1] + left[j - r];
      var temp = ndu[r][j - 1] / ndu[j][r];
      ndu[r][j] = saved + right[r + 1] * temp;
      saved = left[j - r] * temp;
    }
    ndu[j][j] = saved;
  }

  var ders = [[], [], []];
  for (j = 0; j <= p; j++) ders[0][j] = ndu[j][p];

  var a0 = new Array(p + 1);
  var a1 = new Array(p + 1);
  for (i = 0; i <= p; i++) { a0[i] = 0; a1[i] = 0; }

  for (r = 0; r <= p; r++) {
    var s1 = a0;
    var s2 = a1;
    for (j = 0; j <= p; j++) { s1[j] = 0; s2[j] = 0; }
    s1[0] = 1;
    for (k = 1; k <= 2; k++) {
      var d = 0;
      var rk = r - k;
      var pk = p - k;
      if (r >= k) {
        s2[0] = s1[0] / ndu[pk + 1][rk];
        d = s2[0] * ndu[rk][pk];
      }
      var j1 = (rk >= -1) ? 1 : -rk;
      var j2 = (r - 1 <= pk) ? k - 1 : p - r;
      for (j = j1; j <= j2; j++) {
        s2[j] = (s1[j] - s1[j - 1]) / ndu[pk + 1][rk + j];
        d += s2[j] * ndu[rk + j][pk];
      }
      if (r <= pk) {
        s2[k] = -s1[k - 1] / ndu[pk + 1][r];
        d += s2[k] * ndu[r][pk];
      }
      ders[k][r] = d;
      var swap = s1;
      s1 = s2;
      s2 = swap;
    }
  }

  // Scale by p! / (p - k)!.
  for (k = 1; k <= 2; k++) {
    var factor = 1;
    for (j = 0; j < k; j++) factor *= (p - j);
    for (r = 0; r <= p; r++) ders[k][r] *= factor;
  }

  var out1 = new Array(p + 1);
  var out2 = new Array(p + 1);
  for (i = 0; i <= p; i++) {
    out1[i] = ders[1][i];
    out2[i] = ders[2][i];
  }

  return { span: span, vals: ders[0], d1: out1, d2: out2 };
}

//
// Evaluate the rational surface, its first and second partials, and the
// rational basis functions and their derivatives at (u, v). Homogeneous
// quantities carry the weights; the quotient rule then gives the rational
// derivatives exactly, including the mixed terms that arise when both axes
// vary.
//
function evaluateAt (model, u, v) {
  var nu = model.size[0];
  var nv = model.size[1];
  var pu = model.degree[0];
  var pv = model.degree[1];

  var bu = basisDerivatives(u, model.knots[0], nu, pu);
  var bv = basisDerivatives(v, model.knots[1], nv, pv);

  var i0 = bu.span - pu;
  var j0 = bv.span - pv;
  var i, j;

  var hA = [0, 0, 0];
  var hAu = [0, 0, 0];
  var hAv = [0, 0, 0];
  var hAuu = [0, 0, 0];
  var hAuv = [0, 0, 0];
  var hAvv = [0, 0, 0];
  var hW = 0;
  var hWu = 0;
  var hWv = 0;
  var hWuu = 0;
  var hWuv = 0;
  var hWvv = 0;

  var wref = [];
  for (i = 0; i <= pu; i++) {
    wref[i] = [];
    for (j = 0; j <= pv; j++) wref[i][j] = model.weights[i0 + i][j0 + j];
  }

  for (i = 0; i <= pu; i++) {
    var Ni = bu.vals[i];
    var Niu = bu.d1[i];
    var Niuu = bu.d2[i];
    for (j = 0; j <= pv; j++) {
      var Nj = bv.vals[j];
      var Njv = bv.d1[j];
      var Njvv = bv.d2[j];
      var w = wref[i][j];
      var P = model.points[i0 + i][j0 + j];

      var val = Ni * Nj;
      var du = Niu * Nj;
      var dv = Ni * Njv;
      var duu = Niuu * Nj;
      var duv = Niu * Njv;
      var dvv = Ni * Njvv;

      hW += val * w;
      hWu += du * w;
      hWv += dv * w;
      hWuu += duu * w;
      hWuv += duv * w;
      hWvv += dvv * w;

      for (var c = 0; c < 3; c++) {
        var pc = w * P[c];
        hA[c] += val * pc;
        hAu[c] += du * pc;
        hAv[c] += dv * pc;
        hAuu[c] += duu * pc;
        hAuv[c] += duv * pc;
        hAvv[c] += dvv * pc;
      }
    }
  }

  if (!(hW > 0)) fail('degenerate rational weight at an integration point');

  var q = quotientDerivatives(hA, hAu, hAv, hAuu, hAuv, hAvv, hW, hWu, hWv, hWuu, hWuv, hWvv);

  // Rational basis functions: R_ij = w_ij Ni Nj / W, differentiated by the
  // same quotient rule with numerator w_ij Ni Nj.
  var basis = new Array((pu + 1) * (pv + 1));
  var b = 0;
  for (i = 0; i <= pu; i++) {
    var Ni2 = bu.vals[i];
    var Niu2 = bu.d1[i];
    var Niuu2 = bu.d2[i];
    for (j = 0; j <= pv; j++) {
      var Nj2 = bv.vals[j];
      var Njv2 = bv.d1[j];
      var Njvv2 = bv.d2[j];
      var w2 = wref[i][j];
      var n = [w2 * Ni2 * Nj2];
      var nu1 = [w2 * Niu2 * Nj2];
      var nv1 = [w2 * Ni2 * Njv2];
      var nuu = [w2 * Niuu2 * Nj2];
      var nuv = [w2 * Niu2 * Njv2];
      var nvv = [w2 * Ni2 * Njvv2];
      var rq = quotientDerivatives(n, nu1, nv1, nuu, nuv, nvv, hW, hWu, hWv, hWuu, hWuv, hWvv);
      basis[b++] = {
        i: i0 + i,
        j: j0 + j,
        value: rq.S[0],
        du: rq.Su[0],
        dv: rq.Sv[0],
        duu: rq.Suu[0],
        duv: rq.Suv[0],
        dvv: rq.Svv[0]
      };
    }
  }

  return {
    S: q.S,
    Su: q.Su,
    Sv: q.Sv,
    Suu: q.Suu,
    Suv: q.Suv,
    Svv: q.Svv,
    basis: basis
  };
}

//
// Quotient rule for S = A / W and its first two partial derivatives, given
// A and W with their partials. Works componentwise on the numerator vector.
//
function quotientDerivatives (hA, hAu, hAv, hAuu, hAuv, hAvv, W, Wu, Wv, Wuu, Wuv, Wvv) {
  var n = hA.length;
  var S = new Array(n);
  var Su = new Array(n);
  var Sv = new Array(n);
  var Suu = new Array(n);
  var Suv = new Array(n);
  var Svv = new Array(n);
  for (var c = 0; c < n; c++) {
    S[c] = hA[c] / W;
    Su[c] = (hAu[c] - S[c] * Wu) / W;
    Sv[c] = (hAv[c] - S[c] * Wv) / W;
    Suu[c] = (hAuu[c] - 2 * Su[c] * Wu - S[c] * Wuu) / W;
    Suv[c] = (hAuv[c] - Su[c] * Wv - Sv[c] * Wu - S[c] * Wuv) / W;
    Svv[c] = (hAvv[c] - 2 * Sv[c] * Wv - S[c] * Wvv) / W;
  }
  return { S: S, Su: Su, Sv: Sv, Suu: Suu, Suv: Suv, Svv: Svv };
}

function dot (a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function cross (a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  ];
}

//
// Discrete energy of a displacement field given by control point
// displacements D (a size[0] x size[1] x 3 nested array). Uses exactly the
// same quadrature and reference metric as the assembled operator.
//
function energyAt (model, quad, D, lambda) {
  var total = 0;
  var q, c, a, b, aa, bb;

  for (q = 0; q < quad.length; q++) {
    var point = quad[q];
    var metric = point.metric;
    var basis = point.ev.basis;

    var D0 = [0, 0, 0];
    var Du = [0, 0, 0];
    var Dv = [0, 0, 0];
    var Duu = [0, 0, 0];
    var Duv = [0, 0, 0];
    var Dvv = [0, 0, 0];
    for (var k = 0; k < basis.length; k++) {
      var bk = basis[k];
      var dk = D[bk.i][bk.j];
      for (c = 0; c < 3; c++) {
        D0[c] += bk.value * dk[c];
        Du[c] += bk.du * dk[c];
        Dv[c] += bk.dv * dk[c];
        Duu[c] += bk.duu * dk[c];
        Duv[c] += bk.duv * dk[c];
        Dvv[c] += bk.dvv * dk[c];
      }
    }

    var second = [[Duu, Duv], [Duv, Dvv]];
    var first = [Du, Dv];
    var H = [[[0, 0, 0], [0, 0, 0]], [[0, 0, 0], [0, 0, 0]]];
    for (a = 0; a < 2; a++) {
      for (b = 0; b < 2; b++) {
        for (c = 0; c < 3; c++) {
          H[a][b][c] = second[a][b][c] -
            (metric.Gamma[0][a][b] * first[0][c] + metric.Gamma[1][a][b] * first[1][c]);
        }
      }
    }

    var bend = 0;
    for (a = 0; a < 2; a++) {
      for (b = 0; b < 2; b++) {
        for (aa = 0; aa < 2; aa++) {
          for (bb = 0; bb < 2; bb++) {
            bend += metric.gi[a][aa] * metric.gi[b][bb] * dot(H[a][b], H[aa][bb]);
          }
        }
      }
    }
    total += point.weight * metric.area * (bend + lambda * dot(D0, D0));
  }
  return total;
}

//
// Reference metric at an evaluation point, plus the Christoffel symbols and
// the inverse metric used by the energy integrand.
//
function metricAt (ev) {
  var Su = ev.Su;
  var Sv = ev.Sv;
  var nrm = cross(Su, Sv);
  var area = Math.sqrt(dot(nrm, nrm));

  var g11 = dot(Su, Su);
  var g12 = dot(Su, Sv);
  var g22 = dot(Sv, Sv);
  var det = g11 * g22 - g12 * g12;
  if (!(det > 0) || !(area > 0)) fail('degenerate integration point');
  var sin2 = det / (g11 * g22);
  if (!(sin2 >= MIN_TANGENT_SINE * MIN_TANGENT_SINE)) fail('degenerate integration point');

  var gi = [[g22 / det, -g12 / det], [-g12 / det, g11 / det]];

  var S = [ev.Su, ev.Sv];
  var Ss = [ev.Suu, ev.Suv, ev.Svv];
  var Gamma = [[[0, 0], [0, 0]], [[0, 0], [0, 0]]];
  for (var a = 0; a < 2; a++) {
    for (var b = 0; b < 2; b++) {
      var sab = Ss[a + b];
      var d1 = dot(S[0], sab);
      var d2 = dot(S[1], sab);
      Gamma[0][a][b] = gi[0][0] * d1 + gi[0][1] * d2;
      Gamma[1][a][b] = gi[1][0] * d1 + gi[1][1] * d2;
    }
  }

  return { area: area, gi: gi, Gamma: Gamma };
}

//
// Build the reference model: validated deep copies, quadrature points with
// their metric data, and the fixed displacement energy matrix.
//
function buildReference (spline, lambda) {
  var domain = validateSurface(spline);

  var model = {
    points: cloneData(spline.points),
    weights: cloneData(spline.weights),
    knots: cloneData(spline.knots),
    degree: cloneData(spline.degree),
    boundary: cloneData(spline.boundary),
    size: cloneData(spline.size)
  };

  var quad = buildQuadrature(domain, model.knots, model.size, model.degree);
  var evaluated = [];
  var i;

  for (i = 0; i < quad.length; i++) {
    var ev = evaluateAt(model, quad[i].u, quad[i].v);
    evaluated.push({ ev: ev, metric: metricAt(ev), weight: quad[i].weight });
  }

  // Number of control points and rows of the displacement unknown.
  var npts = model.size[0] * model.size[1];
  var nrows = 3 * npts;
  // K is the displacement energy matrix: E = 1/2 d^T K d. The bending term
  // is diagonal in ambient coordinate because the covariant Hessian is
  // applied componentwise; lambda adds the mass term.
  var K = [];
  for (i = 0; i < nrows; i++) K[i] = new Float64Array(nrows);

  var nb = (model.degree[0] + 1) * (model.degree[1] + 1);

  for (var q = 0; q < evaluated.length; q++) {
    var item = evaluated[q];
    var evq = item.ev;
    var metricq = item.metric;
    var wq = item.weight * metricq.area;
    var basis = evq.basis;

    // Per-basis covariant Hessian coefficient tensor
    //   H_ab = R_ab - sum_e Gamma^e_ab R_e.
    // Indexed [k][a * 2 + b].
    var Hcoef = new Array(nb);
    for (var k = 0; k < nb; k++) {
      var bk = basis[k];
      var Hk = new Float64Array(4);
      var Rv = [bk.du, bk.dv];
      var Rab = [bk.duu, bk.duv, bk.duv, bk.dvv];
      for (var a = 0; a < 2; a++) {
        for (var b = 0; b < 2; b++) {
          var idx = a * 2 + b;
          Hk[idx] = Rab[idx] - (metricq.Gamma[0][a][b] * Rv[0] + metricq.Gamma[1][a][b] * Rv[1]);
        }
      }
      Hcoef[k] = Hk;
    }

    // Contraction of the metric with the Hessian index pairs.
    var G = new Float64Array(16);
    for (var a1 = 0; a1 < 2; a1++) {
      for (var b1 = 0; b1 < 2; b1++) {
        for (var e1 = 0; e1 < 2; e1++) {
          for (var f1 = 0; f1 < 2; f1++) {
            G[a1 * 2 + b1 + 4 * (e1 * 2 + f1)] = metricq.gi[a1][e1] * metricq.gi[b1][f1];
          }
        }
      }
    }

    var loc = new Array(nb);
    for (var kk = 0; kk < nb; kk++) {
      loc[kk] = basis[kk].i * model.size[1] + basis[kk].j;
    }

    for (var k1 = 0; k1 < nb; k1++) {
      var rowk = loc[k1];
      var Hk1 = Hcoef[k1];
      var valk = basis[k1].value;
      for (var k2 = 0; k2 < nb; k2++) {
        var rowl = loc[k2];
        var Hl = Hcoef[k2];
        var bend = 0;
        for (a1 = 0; a1 < 2; a1++) {
          for (b1 = 0; b1 < 2; b1++) {
            var h1 = Hk1[a1 * 2 + b1];
            if (h1 === 0) continue;
            for (e1 = 0; e1 < 2; e1++) {
              for (f1 = 0; f1 < 2; f1++) {
                bend += G[a1 * 2 + b1 + 4 * (e1 * 2 + f1)] * h1 * Hl[e1 * 2 + f1];
              }
            }
          }
        }
        // With E = 1/2 d^T A d, the Hessian entry is twice the quadrature
        // coupling sum.
        var coupling = 2 * wq * (bend + lambda * valk * basis[k2].value);
        for (var c = 0; c < 3; c++) {
          K[3 * rowk + c][3 * rowl + c] += coupling;
        }
      }
    }
  }

  return {
    model: model,
    domain: domain,
    quad: evaluated,
    npts: npts,
    nrows: nrows,
    K: K
  };
}

module.exports = {
  GL_X: GL_X,
  GL_W: GL_W,
  MIN_TANGENT_SINE: MIN_TANGENT_SINE,
  cloneData: cloneData,
  validateSurface: validateSurface,
  buildQuadrature: buildQuadrature,
  spanIndex: spanIndex,
  basisDerivatives: basisDerivatives,
  evaluateAt: evaluateAt,
  quotientDerivatives: quotientDerivatives,
  metricAt: metricAt,
  energyAt: energyAt,
  buildReference: buildReference
};
