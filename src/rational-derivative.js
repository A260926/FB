'use strict';

//
// Analytical derivatives (orders 0..2 in each dimension) for rational
// B-splines.
//
// The generated de Boor evaluator handles point values and rational first
// derivatives.  A quotient-rule second derivative additionally needs the
// second derivative of the homogeneous spline, so this module evaluates the
// homogeneous control net and its partial derivatives with the Cox-de Boor
// derivative basis (The NURBS Book, algorithm A2.3) and applies the quotient
// rule analytically.  No finite differences are involved.
//
// The returned evaluator is cached in the same evaluator cache as the generated
// ones (see src/evaluate.js); data is read through the same array-of-arrays /
// ndarray access conventions as src/utils/create-accessors.js.
//

var isArrayLike = require('./utils/is-array-like');

function hasGetter (data) {
  return data && typeof data.get === 'function';
}

function isNdView (data) {
  return data && data.data !== undefined &&
    Array.isArray(data.shape) &&
    data.offset !== undefined &&
    data.stride !== undefined;
}

function readNested (data, indices) {
  var cur = data;
  for (var i = 0; i < indices.length; i++) cur = cur[indices[i]];
  return cur;
}

function readPoint (points, indices, component) {
  if (!points) return undefined;
  if (hasGetter(points)) {
    var args = indices.slice();
    args.push(component);
    return points.get.apply(points, args);
  }
  if (isNdView(points)) {
    var ptr = points.offset;
    for (var i = 0; i < indices.length; i++) ptr += points.stride[i] * indices[i];
    ptr += points.stride[indices.length] * component;
    return points.data[ptr];
  }
  var row = readNested(points, indices);
  return row[component];
}

function readScalar (data, indices) {
  if (!data) return undefined;
  if (hasGetter(data)) {
    return data.get.apply(data, indices);
  }
  if (isNdView(data)) {
    var ptr = data.offset;
    for (var i = 0; i < indices.length; i++) ptr += data.stride[i] * indices[i];
    return data.data[ptr];
  }
  return readNested(data, indices);
}

function hasKnotVector (nurbs, d) {
  var k = nurbs.knots;
  if (!k) return false;
  if (hasGetter(k) || isNdView(k)) return true;
  return isArrayLike(k) && isArrayLike(k[d]);
}

function readKnotAt (nurbs, d, i) {
  var k = nurbs.knots;
  if (hasGetter(k)) return k.get(d, i);
  if (isNdView(k)) {
    return k.data[k.offset + k.stride[0] * d + k.stride[1] * i];
  }
  return k[d][i];
}

function readKnots (nurbs, d, length) {
  var out = new Float64Array(length);
  for (var i = 0; i < length; i++) out[i] = readKnotAt(nurbs, d, i);
  return out;
}

function uniformKnot (boundary, degree, n, j) {
  if (boundary === 'clamped') {
    if (j < degree) return degree;
    if (j > n) return n;
    return j;
  }
  return j;
}

function closedKnot (K, n, j) {
  var period = K[n] - K[0];
  var m = ((j % n) + n) % n;
  var q = Math.floor(j / n);
  return K[m] + q * period;
}

function prepareDimension (nurbs, d) {
  var p = nurbs.degree[d];
  var n = nurbs.size[d];
  var boundary = nurbs.boundary[d];
  var explicitKnots = hasKnotVector(nurbs, d);
  var K = null;
  var knot;

  if (explicitKnots) {
    var length = boundary === 'closed' ? n + 1 : n + p + 1;
    K = readKnots(nurbs, d, length);
    if (boundary === 'closed') {
      knot = function (j) { return closedKnot(K, n, j); };
    } else {
      knot = function (j) { return K[j]; };
    }
  } else {
    knot = function (j) { return uniformKnot(boundary, p, n, j); };
  }

  var domain = boundary === 'closed'
    ? (explicitKnots ? [K[0], K[n]] : [0, n])
    : (explicitKnots ? [K[p], K[n]] : [p, n]);

  return {
    index: d,
    degree: p,
    size: n,
    boundary: boundary,
    knot: knot,
    domain: domain
  };
}

function findSpan (dim, t) {
  var n = dim.size;
  var p = dim.degree;
  var i;

  if (dim.boundary === 'closed') {
    i = t >= dim.domain[1] ? n - 1 : 0;
    if (t < dim.domain[1]) {
      var klo = 0;
      var khi = n - 1;
      while (klo < khi) {
        var kmid = (klo + khi + 1) >> 1;
        if (dim.knot(kmid) <= t) klo = kmid; else khi = kmid - 1;
      }
      i = klo;
    }
  } else {
    i = t >= dim.domain[1] ? n - 1 : p;
    if (t < dim.domain[1]) {
      var low = p;
      var high = n - 1;
      while (low < high) {
        var mid = (low + high + 1) >> 1;
        if (dim.knot(mid) <= t) low = mid; else high = mid - 1;
      }
      i = low;
    }
  }

  // Skip zero-width spans (e.g. an over-clamped knot vector whose final span
  // collapsed onto the domain end).
  while (i > 0 && dim.knot(i + 1) === dim.knot(i)) i--;
  return i;
}

// Basis values and derivatives of order 1..2 at `t`, in the supplied span.
// This is "ders_basis_funs" (The NURBS Book A2.3) restricted to two orders.
function dersBasisFuns (span, t, p, knot) {
  var i, j, r, k;
  var ndu = [];
  var left = new Array(p + 1);
  var right = new Array(p + 1);
  for (i = 0; i <= p; i++) ndu.push(new Array(p + 1).fill(0));

  ndu[0][0] = 1;
  for (j = 1; j <= p; j++) {
    left[j] = t - knot(span + 1 - j);
    right[j] = knot(span + j) - t;
    var saved = 0;
    for (r = 0; r < j; r++) {
      ndu[j][r] = right[r + 1] + left[j - r];
      var temp = ndu[j][r] === 0 ? 0 : ndu[r][j - 1] / ndu[j][r];
      ndu[r][j] = saved + right[r + 1] * temp;
      saved = left[j - r] * temp;
    }
    ndu[j][j] = saved;
  }

  var values = new Array(p + 1);
  var first = new Array(p + 1).fill(0);
  var second = new Array(p + 1).fill(0);
  for (j = 0; j <= p; j++) values[j] = ndu[j][p];

  var maxOrder = Math.min(2, p);
  if (maxOrder === 0) return { values: values, first: first, second: second };

  var aa = [new Array(p + 1).fill(0), new Array(p + 1).fill(0)];
  var raw = [null, new Array(p + 1).fill(0), new Array(p + 1).fill(0)];

  for (r = 0; r <= p; r++) {
    var s1 = 0;
    var s2 = 1;
    aa[0][0] = 1;
    for (k = 1; k <= maxOrder; k++) {
      var d = 0;
      var rk = r - k;
      var pk = p - k;
      if (r >= k) {
        aa[s2][0] = aa[s1][0] / ndu[pk + 1][rk];
        d = aa[s2][0] * ndu[rk][pk];
      }
      var j1 = rk >= -1 ? 1 : -rk;
      var j2 = (r - 1 <= pk) ? k - 1 : p - r;
      for (j = j1; j <= j2; j++) {
        aa[s2][j] = (aa[s1][j] - aa[s1][j - 1]) / ndu[pk + 1][rk + j];
        d += aa[s2][j] * ndu[rk + j][pk];
      }
      if (r <= pk) {
        aa[s2][k] = -aa[s1][k - 1] / ndu[pk + 1][r];
        d += aa[s2][k] * ndu[r][pk];
      }
      raw[k][r] = d;
      var swap = s1;
      s1 = s2;
      s2 = swap;
    }
  }

  var factor = p;
  for (k = 1; k <= maxOrder; k++) {
    for (j = 0; j <= p; j++) raw[k][j] *= factor;
    factor *= (p - k);
  }

  for (j = 0; j <= p; j++) {
    first[j] = raw[1][j];
    second[j] = maxOrder >= 2 ? raw[2][j] : 0;
  }
  return { values: values, first: first, second: second };
}

function derivativeRow (info, order) {
  if (order === 0) return info.values;
  if (order === 1) return info.first;
  return info.second;
}

function createEvaluator (derivative, isBasis) {
  var order = derivative;

  return function () {
    var nurbs = this;
    var splineDimension = nurbs.splineDimension;
    if (splineDimension > 2) {
      throw new Error('Analytical second derivatives not implemented for rational splines of dimension ' + splineDimension + '.');
    }

    var out = null;
    var offset = 0;
    if (!isBasis) {
      out = arguments[0];
      offset = 1;
    }

    var dims = [prepareDimension(nurbs, 0)];
    if (splineDimension > 1) dims.push(prepareDimension(nurbs, 1));

    if (nurbs.checkBounds) {
      var domain = nurbs.domain;
      for (var cd = 0; cd < splineDimension; cd++) {
        var param = arguments[cd + offset];
        if (param < domain[cd][0] || param > domain[cd][1] || param === undefined || isNaN(param)) {
          throw new Error('Invalid Spline parameter in dimension ' + cd + '. Valid domain is [' + domain[cd][0] + ', ' + domain[cd][1] + ']. but got t' + cd + ' = ' + param + '.');
        }
      }
    }

    var t = [];
    for (var td = 0; td < splineDimension; td++) t[td] = arguments[td + offset];

    var spatialDimension = nurbs.dimension;
    var hasWeights = nurbs.weights !== undefined;
    var i, j;

    var size = nurbs.size;
    var n0 = size[0];
    var n1 = splineDimension > 1 ? size[1] : 1;
    var total = n0 * n1;

    // Homogeneous control net: [x0*w, x1*w, ..., w].
    var homogeneous = new Float64Array(total * (spatialDimension + 1));
    var weights = new Float64Array(total);

    for (i = 0; i < n0; i++) {
      for (j = 0; j < n1; j++) {
        var flat = splineDimension > 1 ? i * n1 + j : i;
        var w = hasWeights ? readScalar(nurbs.weights, splineDimension > 1 ? [i, j] : [i]) : 1;
        weights[flat] = w;
        for (var d = 0; d < spatialDimension; d++) {
          var x = readPoint(nurbs.points, splineDimension > 1 ? [i, j] : [i], d);
          homogeneous[flat * (spatialDimension + 1) + d] = x * w;
        }
        homogeneous[flat * (spatialDimension + 1) + spatialDimension] = w;
      }
    }

    var basisTarget = null;
    if (isBasis) {
      basisTarget = [];
      for (var bi = 0; bi < splineDimension; bi++) {
        basisTarget[bi] = arguments[offset + splineDimension + bi];
      }
    }

    var span0 = findSpan(dims[0], t[0]);
    var span1 = splineDimension > 1 ? findSpan(dims[1], t[1]) : 0;

    var wrap0 = dims[0].boundary === 'closed';
    var wrap1 = splineDimension > 1 && dims[1].boundary === 'closed';

    function wrapIndex (idx, dim, wrap) {
      if (!wrap) return idx;
      var n = dim.size;
      return ((idx % n) + n) % n;
    }

    function matchesTarget (idx, dim, wrap, target) {
      if (wrap) {
        var n = dim.size;
        return (((idx % n) + n) % n) === (((target % n) + n) % n);
      }
      return idx === target;
    }

    function flatIndex (ii, jj) {
      var mi = wrapIndex(ii, dims[0], wrap0);
      var mj = splineDimension > 1 ? wrapIndex(jj, dims[1], wrap1) : 0;
      return splineDimension > 1 ? mi * n1 + mj : mi;
    }

    function homValues (ii, jj) {
      var base = flatIndex(ii, jj) * (spatialDimension + 1);
      var vec = new Array(spatialDimension + 1);
      for (var k = 0; k <= spatialDimension; k++) vec[k] = homogeneous[base + k];
      return vec;
    }

    function weightValue (ii, jj) {
      return weights[flatIndex(ii, jj)];
    }

    function basisNumerator (ii, jj) {
      var matchI = matchesTarget(ii, dims[0], wrap0, basisTarget[0]);
      var matchJ = splineDimension > 1
        ? matchesTarget(jj, dims[1], wrap1, basisTarget[1])
        : true;
      return matchI && matchJ ? weights[flatIndex(ii, jj)] : 0;
    }

    var p0 = dims[0].degree;
    var p1 = splineDimension > 1 ? dims[1].degree : 0;

    var basis0 = dersBasisFuns(span0, t[0], p0, dims[0].knot);
    var basis1 = splineDimension > 1
      ? dersBasisFuns(span1, t[1], p1, dims[1].knot)
      : { values: [1], first: [0], second: [0] };

    // Evaluate a tensor-product derivative of a per-control-point quantity.
    // `reader(ii, jj)` returns an array of length `width`.
    function evaluateDerivative (a, b, reader, width) {
      var sum = new Array(width).fill(0);
      if (a > p0 || b > p1) return sum;
      var rowU = derivativeRow(basis0, a);
      var rowV = derivativeRow(basis1, b);
      for (var s = 0; s <= p0; s++) {
        var cu = rowU[s];
        if (cu === 0) continue;
        var ii = span0 - p0 + s;
        for (var r = 0; r <= p1; r++) {
          var cv = rowV[r];
          if (cv === 0) continue;
          var jj = splineDimension > 1 ? span1 - p1 + r : 0;
          var coeff = cu * cv;
          var value = reader(ii, jj);
          for (var k = 0; k < width; k++) sum[k] += coeff * value[k];
        }
      }
      return sum;
    }

    function scalarReader (fn) {
      return function (ii, jj) { return [fn(ii, jj)]; };
    }

    var a = order[0] || 0;
    var b = splineDimension > 1 ? (order[1] || 0) : 0;

    var H0 = evaluateDerivative(0, 0, homValues, spatialDimension + 1);
    var W0 = evaluateDerivative(0, 0, scalarReader(weightValue), 1)[0];

    if (isBasis) {
      var B0 = evaluateDerivative(0, 0, scalarReader(basisNumerator), 1)[0];
      var R = B0 / W0;
      var R1u = 0;
      var R1v = 0;
      var W1u = 0;
      var W1v = 0;
      if (a === 2 && b === 0) {
        W1u = evaluateDerivative(1, 0, scalarReader(weightValue), 1)[0];
        R1u = (evaluateDerivative(1, 0, scalarReader(basisNumerator), 1)[0] - R * W1u) / W0;
        var B2u = evaluateDerivative(2, 0, scalarReader(basisNumerator), 1)[0];
        var W2u = evaluateDerivative(2, 0, scalarReader(weightValue), 1)[0];
        return (B2u - 2 * R1u * W1u - R * W2u) / W0;
      }
      if (a === 0 && b === 2) {
        W1v = evaluateDerivative(0, 1, scalarReader(weightValue), 1)[0];
        R1v = (evaluateDerivative(0, 1, scalarReader(basisNumerator), 1)[0] - R * W1v) / W0;
        var B2v = evaluateDerivative(0, 2, scalarReader(basisNumerator), 1)[0];
        var W2v = evaluateDerivative(0, 2, scalarReader(weightValue), 1)[0];
        return (B2v - 2 * R1v * W1v - R * W2v) / W0;
      }
      if (a === 1 && b === 1) {
        W1u = evaluateDerivative(1, 0, scalarReader(weightValue), 1)[0];
        W1v = evaluateDerivative(0, 1, scalarReader(weightValue), 1)[0];
        R1u = (evaluateDerivative(1, 0, scalarReader(basisNumerator), 1)[0] - R * W1u) / W0;
        R1v = (evaluateDerivative(0, 1, scalarReader(basisNumerator), 1)[0] - R * W1v) / W0;
        var Buv = evaluateDerivative(1, 1, scalarReader(basisNumerator), 1)[0];
        var Wuv = evaluateDerivative(1, 1, scalarReader(weightValue), 1)[0];
        return (Buv - R1u * W1v - R1v * W1u - R * Wuv) / W0;
      }
      if (a === 1 && b === 0) {
        W1u = evaluateDerivative(1, 0, scalarReader(weightValue), 1)[0];
        return (evaluateDerivative(1, 0, scalarReader(basisNumerator), 1)[0] - R * W1u) / W0;
      }
      if (a === 0 && b === 1) {
        W1v = evaluateDerivative(0, 1, scalarReader(weightValue), 1)[0];
        return (evaluateDerivative(0, 1, scalarReader(basisNumerator), 1)[0] - R * W1v) / W0;
      }
      return R;
    }

    var C = new Array(spatialDimension);
    for (i = 0; i < spatialDimension; i++) C[i] = H0[i] / W0;

    var C1u = null;
    var C1v = null;
    var W1uValue = 0;
    var W1vValue = 0;
    if (a > 0) {
      var Hu = evaluateDerivative(1, 0, homValues, spatialDimension + 1);
      W1uValue = evaluateDerivative(1, 0, scalarReader(weightValue), 1)[0];
      C1u = new Array(spatialDimension);
      for (i = 0; i < spatialDimension; i++) C1u[i] = (Hu[i] - C[i] * W1uValue) / W0;
    }
    if (b > 0) {
      var Hv = evaluateDerivative(0, 1, homValues, spatialDimension + 1);
      W1vValue = evaluateDerivative(0, 1, scalarReader(weightValue), 1)[0];
      C1v = new Array(spatialDimension);
      for (i = 0; i < spatialDimension; i++) C1v[i] = (Hv[i] - C[i] * W1vValue) / W0;
    }

    if (a === 2 && b === 0) {
      var Huu = evaluateDerivative(2, 0, homValues, spatialDimension + 1);
      var Wuu = evaluateDerivative(2, 0, scalarReader(weightValue), 1)[0];
      for (i = 0; i < spatialDimension; i++) {
        out[i] = (Huu[i] - 2 * C1u[i] * W1uValue - C[i] * Wuu) / W0;
      }
    } else if (a === 0 && b === 2) {
      var Hvv = evaluateDerivative(0, 2, homValues, spatialDimension + 1);
      var Wvv = evaluateDerivative(0, 2, scalarReader(weightValue), 1)[0];
      for (i = 0; i < spatialDimension; i++) {
        out[i] = (Hvv[i] - 2 * C1v[i] * W1vValue - C[i] * Wvv) / W0;
      }
    } else if (a === 1 && b === 1) {
      var Huv = evaluateDerivative(1, 1, homValues, spatialDimension + 1);
      var Wuv2 = evaluateDerivative(1, 1, scalarReader(weightValue), 1)[0];
      for (i = 0; i < spatialDimension; i++) {
        out[i] = (Huv[i] - C1u[i] * W1vValue - C1v[i] * W1uValue - C[i] * Wuv2) / W0;
      }
    } else if (a === 1 && b === 0) {
      for (i = 0; i < spatialDimension; i++) out[i] = C1u[i];
    } else if (a === 0 && b === 1) {
      for (i = 0; i < spatialDimension; i++) out[i] = C1v[i];
    } else {
      for (i = 0; i < spatialDimension; i++) out[i] = C[i];
    }
    return out;
  };
}

module.exports = createEvaluator;
