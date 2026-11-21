'use strict';

//
// Rational derivatives of total order one and two.
//
// The generated evaluator handles rational values and, for curves, generic
// first derivatives. The tensor product case needs an explicit homogeneous
// evaluation so that mixed derivative orders (including the v direction and
// the mixed uv partial) follow the quotient rule exactly:
//
//   S    = A / W
//   S_a  = (A_a - S W_a) / W
//   S_ab = (A_ab - S_a W_b - S_b W_a - S W_ab) / W
//
// where A and W are the homogeneous numerator vector and denominator of the
// rational spline differentiated term by term with the B-spline basis
// derivatives. src/evaluate.js delegates to this module for rational splines
// of one or two parameters when at least one derivative order is requested.
//

var inferType = require('./utils/infer-type');

// ---------------------------------------------------------------------------
// Knot vectors and basis derivatives
// ---------------------------------------------------------------------------

function effectiveKnots (nurbs, dim) {
  var knots = nurbs.knots;
  var p = nurbs.degree[dim];
  var n = nurbs.size[dim];
  if (!(n > 0) || !(p >= 0)) return null;
  if (Array.isArray(knots) && Array.isArray(knots[dim])) {
    var explicit = new Array(n + p + 1);
    for (var i = 0; i < explicit.length; i++) explicit[i] = knots[dim][i];
    return explicit;
  }
  var k = new Array(n + p + 1);
  // Implicit uniform knots are the integers 0..n+p; clamped repeats the first
  // and last p + 1 values, and closed wrapping is not supported here.
  for (i = 0; i < k.length; i++) k[i] = i;
  if (nurbs.boundary[dim] === 'clamped') {
    for (i = 0; i < k.length; i++) {
      if (k[i] < p) k[i] = p;
      if (k[i] > n) k[i] = n;
    }
  } else if (nurbs.boundary[dim] === 'closed') {
    return null;
  }
  return k;
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

// Active basis functions (p + 1 of them) with derivatives of order 0, 1, 2.
function axisBasis (u, knots, size, degree) {
  var p = degree;
  var span = spanIndex(u, knots, size, degree);
  var i, j, k, q;

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
    for (var r = 0; r < j; r++) {
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

  for (var r2 = 0; r2 <= p; r2++) {
    var s1 = a0;
    var s2 = a1;
    for (j = 0; j <= p; j++) { s1[j] = 0; s2[j] = 0; }
    s1[0] = 1;
    for (k = 1; k <= 2; k++) {
      var d = 0;
      var rk = r2 - k;
      var pk = p - k;
      if (r2 >= k) {
        s2[0] = s1[0] / ndu[pk + 1][rk];
        d = s2[0] * ndu[rk][pk];
      }
      var j1 = (rk >= -1) ? 1 : -rk;
      var j2 = (r2 - 1 <= pk) ? k - 1 : p - r2;
      for (j = j1; j <= j2; j++) {
        s2[j] = (s1[j] - s1[j - 1]) / ndu[pk + 1][rk + j];
        d += s2[j] * ndu[rk + j][pk];
      }
      if (r2 <= pk) {
        s2[k] = -s1[k - 1] / ndu[pk + 1][r2];
        d += s2[k] * ndu[r2][pk];
      }
      ders[k][r2] = d;
      var swap = s1;
      s1 = s2;
      s2 = swap;
    }
  }
  for (k = 1; k <= 2; k++) {
    var factor = 1;
    for (j = 0; j < k; j++) factor *= (p - j);
    for (q = 0; q <= p; q++) ders[k][q] *= factor;
  }

  var vals = new Array(p + 1);
  var d1 = new Array(p + 1);
  var d2 = new Array(p + 1);
  for (i = 0; i <= p; i++) {
    vals[i] = ders[0][i];
    d1[i] = ders[1][i];
    d2[i] = ders[2][i];
  }
  return { span: span, vals: vals, d1: d1, d2: d2 };
}

// ---------------------------------------------------------------------------
// Storage access
// ---------------------------------------------------------------------------

function accessorFor (data, splineDimension, spaceDimension) {
  var type = inferType(data);
  var rank = spaceDimension === undefined ? splineDimension : splineDimension + 1;
  if (type === inferType.ARRAY_OF_ARRAYS) {
    return function (idx) {
      var v = data;
      for (var d = 0; d < rank; d++) v = v[idx[d]];
      return v;
    };
  }
  if (type === inferType.NDARRAY) {
    return function (idx) {
      var offset = data.offset;
      for (var d = 0; d < rank; d++) offset += data.stride[d] * idx[d];
      return data.data[offset];
    };
  }
  if (type === inferType.GENERIC_NDARRAY) {
    return function (idx) {
      return data.get.apply(data, idx.slice(0, rank));
    };
  }
  return null;
}

// ---------------------------------------------------------------------------
// Tensor product homogeneous partials
// ---------------------------------------------------------------------------

var PARTIALS = ['value', 'u', 'v', 'uu', 'uv', 'vv'];

function computePacket (nurbs, u, v, isBasis, index) {
  var sd = nurbs.splineDimension;
  var dim = nurbs.dimension;
  var p0 = nurbs.degree[0];
  var p1 = nurbs.degree[1];
  var n0 = nurbs.size[0];
  var n1 = nurbs.size[1];
  var k0 = effectiveKnots(nurbs, 0);
  var k1 = effectiveKnots(nurbs, 1);
  if (!k0 || !k1) return null;

  var bu = axisBasis(u, k0, n0, p0);
  var bv = axisBasis(v, k1, n1, p1);
  var i0 = bu.span - p0;
  var j0 = bv.span - p1;

  var pointAt = accessorFor(nurbs.points, sd, dim);
  var weightAt = accessorFor(nurbs.weights, sd);
  if (!pointAt || !weightAt) return null;

  // Homogeneous partials: value, u, v, uu, uv, vv.
  var W = [0, 0, 0, 0, 0, 0];
  var A = [[0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0], [0, 0, 0, 0, 0, 0]];
  var basis = null;

  for (var i = 0; i <= p0; i++) {
    for (var j = 0; j <= p1; j++) {
      var gi = i0 + i;
      var gj = j0 + j;
      var w = weightAt([gi, gj]);

      var v0 = bu.vals[i] * bv.vals[j];
      var vu = bu.d1[i] * bv.vals[j];
      var vv = bu.vals[i] * bv.d1[j];
      var vuu = bu.d2[i] * bv.vals[j];
      var vuv = bu.d1[i] * bv.d1[j];
      var vvv = bu.vals[i] * bv.d2[j];

      W[0] += w * v0;
      W[1] += w * vu;
      W[2] += w * vv;
      W[3] += w * vuu;
      W[4] += w * vuv;
      W[5] += w * vvv;

      if (isBasis) {
        if (!basis && gi === index[0] && gj === index[1]) {
          basis = [w * v0, w * vu, w * vv, w * vuu, w * vuv, w * vvv];
        }
      } else {
        for (var c = 0; c < dim; c++) {
          var pc = w * pointAt([gi, gj, c]);
          A[c][0] += pc * v0;
          A[c][1] += pc * vu;
          A[c][2] += pc * vv;
          A[c][3] += pc * vuu;
          A[c][4] += pc * vuv;
          A[c][5] += pc * vvv;
        }
      }
    }
  }

  if (nurbs.dimension === 2) A.length = 2;
  if (nurbs.dimension === 1) A.length = 1;

  return { W: W, A: A, basis: basis };
}

// Scalar quotient rule for one numerator vector.
function quotient (num, W) {
  var n = num[0].length;
  var S = new Array(n);
  var Su = new Array(n);
  var Sv = new Array(n);
  var Suu = new Array(n);
  var Suv = new Array(n);
  var Svv = new Array(n);
  var zero = new Array(n);
  for (var z = 0; z < n; z++) zero[z] = 0;
  var n1 = num[1] || zero;
  var n2 = num[2] || zero;
  var n3 = num[3] || zero;
  var n4 = num[4] || zero;
  var n5 = num[5] || zero;
  for (var c = 0; c < n; c++) {
    S[c] = num[0][c] / W[0];
    Su[c] = (n1[c] - S[c] * W[1]) / W[0];
    Sv[c] = (n2[c] - S[c] * W[2]) / W[0];
    Suu[c] = (n3[c] - 2 * Su[c] * W[1] - S[c] * W[3]) / W[0];
    Suv[c] = (n4[c] - Su[c] * W[2] - Sv[c] * W[1] - S[c] * W[4]) / W[0];
    Svv[c] = (n5[c] - 2 * Sv[c] * W[2] - S[c] * W[5]) / W[0];
  }
  return { S: S, Su: Su, Sv: Sv, Suu: Suu, Suv: Suv, Svv: Svv };
}

// ---------------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------------

function createRationalDerivative (nurbs, checkBounds, isBasis, derivative) {
  var sd = nurbs.splineDimension;
  if (sd !== 1 && sd !== 2) return null;
  var du = derivative[0] || 0;
  var dv = sd > 1 ? (derivative[1] || 0) : 0;
  if (!isBasis && sd === 1 && du > 1) return null;

  return function () {
    var offset = isBasis ? 0 : 1;
    var u = arguments[offset];
    var v = sd > 1 ? arguments[offset + 1] : 0;
    if (checkBounds) {
      var domain = nurbs.domain;
      var params = sd > 1 ? [u, v] : [u];
      for (var b = 0; b < sd; b++) {
        var a = params[b];
        if (a < domain[b][0] || a > domain[b][1] || a === undefined || isNaN(a)) {
          throw new Error('Invalid Spline parameter in dimension ' + b + '. Valid domain is [' + domain[b][0] + ', ' + domain[b][1] + '] but got ' + a + '.');
        }
      }
    }
    var index = isBasis ? [arguments[sd], arguments[sd + 1]] : null;
    var packet = computePacket(nurbs, u, v, isBasis, index);
    if (!packet || packet.W[0] === 0) throw new Error('Unsupported rational derivative input');

    var values;
    if (isBasis) {
      if (!packet.basis) return 0;
      // A single scalar numerator: one component per partial order.
      var num = [];
      for (var p = 0; p < packet.basis.length; p++) num.push([packet.basis[p]]);
      var qb = quotient(num, packet.W);
      values = du === 2 ? qb.Suu : dv === 2 ? qb.Svv : (du === 1 && dv === 1 ? qb.Suv : (du === 1 ? qb.Su : (dv === 1 ? qb.Sv : qb.S)));
      return values[0];
    }

    // packet.A is indexed [component][partial]; quotient wants
    // [partial][component].
    var transposed = [];
    for (var p2 = 0; p2 < 6; p2++) {
      var row = [];
      for (var c = 0; c < nurbs.dimension; c++) row.push(packet.A[c][p2]);
      transposed.push(row);
    }
    var q = quotient(transposed, packet.W);
    values = du === 2 ? q.Suu : dv === 2 ? q.Svv : (du === 1 && dv === 1 ? q.Suv : (du === 1 ? q.Su : (dv === 1 ? q.Sv : q.S)));
    var out = arguments[0];
    for (var k = 0; k < nurbs.dimension; k++) out[k] = values[k];
    return out;
  };
}

module.exports = createRationalDerivative;
module.exports.PARTIALS = PARTIALS;
