'use strict';

var inferType = require('./utils/infer-type');
var isArrayLike = require('./utils/is-array-like');
var ndloop = require('./utils/ndloop');

// Create a getter/setter pair for a control net stored either as nested
// arrays or as an ndarray. `indexDimensions` is the number of indices used
// to address a scalar (splineDimension + 1 for points, splineDimension for
// weights). Ndarray views with arbitrary offset and strides (including
// transposed and negatively strided views) are addressed through their
// data/offset/stride triple so that only the values covered by the view
// are touched and the underlying storage identity is preserved.
function createStorage (data, indexDimensions, label) {
  switch (inferType(data)) {
    case inferType.ARRAY_OF_ARRAYS:
      return {
        get: function (idx) {
          var value = data;
          for (var k = 0; k < indexDimensions; k++) value = value[idx[k]];
          return value;
        },
        set: function (idx, value) {
          var target = data;
          for (var k = 0; k < indexDimensions - 1; k++) target = target[idx[k]];
          target[idx[indexDimensions - 1]] = value;
        }
      };
    case inferType.NDARRAY:
      var buffer = data.data;
      var offset = data.offset;
      var stride = data.stride;
      return {
        get: function (idx) {
          var ptr = offset;
          for (var k = 0; k < indexDimensions; k++) ptr += stride[k] * idx[k];
          return buffer[ptr];
        },
        set: function (idx, value) {
          var ptr = offset;
          for (var k = 0; k < indexDimensions; k++) ptr += stride[k] * idx[k];
          buffer[ptr] = value;
        }
      };
    case inferType.GENERIC_NDARRAY:
      throw new Error('spline.projectiveTransform does not support generic ndarray ' + label + ' storage');
    default:
      throw new Error('spline.projectiveTransform requires array of arrays or ndarray ' + label);
  }
}

// Allocate an owning nested-array control net of the given shape. The
// leaves are numbers, so the result has one fewer dimension than the
// point net (it does not borrow the coordinate component).
function allocateWeights (size) {
  var dimension = size.length;
  function allocate (d) {
    var arr = new Array(size[d]);
    for (var i = 0; i < arr.length; i++) {
      arr[i] = d === dimension - 1 ? 0 : allocate(d + 1);
    }
    return arr;
  }
  return allocate(0);
}

// Apply a projective (homogeneous) transformation to a NURBS curve or
// surface exactly, by transforming the homogeneous control points
// (w * P, w) with the (dimension + 1) x (dimension + 1) column-major
// matrix `m` and renormalizing. The degree, knots, boundary, domain and
// number of control points are untouched. Non-rational splines are
// promoted to rational splines with freshly allocated weights.
//
// The full validation pass runs before any value is written, so when a
// RangeError is thrown the control points, weights and evaluator state
// are left untouched.
module.exports = function projectiveTransform (nurbs, m) {
  var splineDimension = nurbs.splineDimension;
  var dimension = nurbs.dimension;
  var size = nurbs.size;
  var n = dimension + 1;
  var i, r, c;

  if (!isArrayLike(m) || m.length !== n * n) {
    throw new RangeError('Expected a ' + n + 'x' + n + ' homogeneous transformation matrix, got length ' + (m && m.length));
  }
  for (i = 0; i < n * n; i++) {
    if (!isFinite(m[i])) {
      throw new RangeError('Transformation matrix entries must be finite, got ' + m[i]);
    }
  }

  var points = createStorage(nurbs.points, splineDimension + 1, 'points');
  var weights = nurbs.weights ? createStorage(nurbs.weights, splineDimension, 'weights') : null;

  var count = 1;
  for (i = 0; i < splineDimension; i++) count *= size[i];

  var homogeneous = new Float64Array(count * n);
  var h = new Float64Array(n);
  var pointIndex = new Array(splineDimension + 1);
  var sign = 0;
  var ptr = 0;

  ndloop(size, function (idx) {
    var w = 1;
    if (weights) {
      w = weights.get(idx);
      if (!isFinite(w) || w <= 0) {
        throw new RangeError('Weights must be positive finite values, got ' + w);
      }
    }
    for (i = 0; i < splineDimension; i++) pointIndex[i] = idx[i];
    for (c = 0; c < dimension; c++) {
      pointIndex[splineDimension] = c;
      var x = points.get(pointIndex);
      if (!isFinite(x)) {
        throw new RangeError('Control points must be finite, got ' + x);
      }
      h[c] = w * x;
    }
    h[dimension] = w;

    for (r = 0; r < n; r++) {
      var sum = 0;
      for (c = 0; c < n; c++) sum += m[c * n + r] * h[c];
      if (!isFinite(sum)) {
        throw new RangeError('Projective transform produced a non-finite result');
      }
      homogeneous[ptr + r] = sum;
    }

    var denominator = homogeneous[ptr + dimension];
    if (denominator === 0) {
      throw new RangeError('Projective transform maps a control point to infinity (zero homogeneous denominator)');
    }
    var denominatorSign = denominator > 0 ? 1 : -1;
    if (sign === 0) {
      sign = denominatorSign;
    } else if (denominatorSign !== sign) {
      throw new RangeError('Projective transform produces homogeneous denominators with mixed signs');
    }

    ptr += n;
  });

  // Validation has passed, so no errors may be thrown past this point.
  // If all denominators are negative, flip the global sign so that the
  // resulting weights are all positive; the normalized control points
  // are invariant under this flip.
  var newWeights = weights ? null : allocateWeights(size);
  var weightStorage = weights || createStorage(newWeights, splineDimension, 'weights');

  ptr = 0;
  ndloop(size, function (idx) {
    var denominator = homogeneous[ptr + dimension];
    for (i = 0; i < splineDimension; i++) pointIndex[i] = idx[i];
    for (c = 0; c < dimension; c++) {
      pointIndex[splineDimension] = c;
      points.set(pointIndex, homogeneous[ptr + c] / denominator);
    }
    weightStorage.set(idx, sign * denominator);
    ptr += n;
  });

  if (newWeights) nurbs.weights = newWeights;

  return nurbs;
};
