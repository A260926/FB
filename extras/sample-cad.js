'use strict';

//
// CAD-oriented sampler for NURBS surfaces.
//
// `extras/sample.js` treats the control hull as a plain rectangular grid.  CAD
// surfaces frequently have "polar" edges: an entire boundary row of control
// points maps to one spatial point (a dome top, a cone tip, ...).  Sampling
// such a hull as a rectangle leaves overlapping pole vertices, zero-area faces
// and zero normals.
//
// `sampleCad(spline, options)` instead returns independent flat arrays:
//
//   { positions, cells, cornerUVs, cornerNormals }
//
//   - positions: flat xyz triples
//   - cells: flat triangle indices (three per triangle)
//   - cornerUVs: flat uv pairs, one pair per triangle corner
//   - cornerNormals: flat unit xyz triples, one per triangle corner
//
// Behaviour:
//
//   - sample stations live on every non-zero knot interval of each axis'
//     effective domain, split into `resolution[axis]` segments; shared knot
//     values appear once;
//   - ordinary cells split along the lower-left -> upper-right diagonal into
//     two triangles with positive UV winding;
//   - a declared collapsed edge contributes one shared position index, its
//     adjacent ring becomes a fan and each adjacent cell keeps a single
//     non-degenerate triangle;
//   - periodic axes share the first/last station position index while the
//     corner UVs keep the original domain ends;
//   - pole normals are the analytic one-sided limit of Pu x Pv at the corner
//     UV (never a triangle normal or a finite difference).
//

var EDGE_NAMES = ['umin', 'umax', 'vmin', 'vmax'];
var MIN_RESOLUTION = 1;
var MAX_RESOLUTION = 32;
var MIN_RELATIVE_INTERVAL = 1e-4;

// Analytic homogeneous quotient-rule derivatives (orders 0..2) for rational
// splines.  Positions are read through the spline object; tangents and limit
// normals come from this consistent quotient rule.
var createAnalyticDerivatives = require('../src/rational-derivative');

function fail (message) {
  throw new RangeError('sampleCad: ' + message);
}

function isFiniteNumber (x) {
  return typeof x === 'number' && isFinite(x);
}

function isInteger (x) {
  return typeof x === 'number' && isFinite(x) && Math.floor(x) === x;
}

function isNestedArray (x) {
  return Array.isArray(x);
}

function cross (a, b) {
  return [
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
  ];
}

// Normalize, throwing RangeError when the vector is not finite or cannot be
// unitized.  The supported range requires a non-zero, finite cross product.
function normalizeChecked (v, context) {
  if (!isFiniteNumber(v[0]) || !isFiniteNumber(v[1]) || !isFiniteNumber(v[2])) {
    fail('non-finite ' + context);
  }
  var len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  if (!(len2 > 0) || !isFiniteNumber(len2)) {
    fail('degenerate ' + context);
  }
  var inv = 1 / Math.sqrt(len2);
  var out = [v[0] * inv, v[1] * inv, v[2] * inv];
  if (!isFiniteNumber(out[0]) || !isFiniteNumber(out[1]) || !isFiniteNumber(out[2])) {
    fail('non-finite ' + context);
  }
  return out;
}

//
// Structural validation.  Everything here runs before any spline evaluation so
// that broken input is reported as a RangeError rather than surfacing later as
// a NaN or an upstream evaluator error.
//
function validateStructure (spline) {
  if (!spline || (typeof spline !== 'object' && typeof spline !== 'function')) {
    fail('expected a nurbs spline object');
  }
  if (spline.splineDimension !== 2) {
    fail('only two-parameter surfaces are supported');
  }
  if (spline.dimension !== 3) {
    fail('only three-dimensional control points are supported');
  }

  var degree = spline.degree;
  if (!isNestedArray(degree) || degree.length !== 2) {
    fail('degree must be an array of two numbers');
  }
  for (var d = 0; d < 2; d++) {
    if (degree[d] !== 2 && degree[d] !== 3) {
      fail('only degree 2 or 3 is supported');
    }
  }

  var points = spline.points;
  if (!isNestedArray(points) || points.length === 0) {
    fail('control points must be a nested array');
  }
  var n0 = points.length;
  var n1 = null;
  var i, j, k;
  for (i = 0; i < n0; i++) {
    if (!isNestedArray(points[i])) {
      fail('control points must be a nested array');
    }
    if (n1 === null) n1 = points[i].length;
    if (n1 === 0 || points[i].length !== n1) {
      fail('control net must be rectangular');
    }
    for (j = 0; j < n1; j++) {
      var point = points[i][j];
      if (!isNestedArray(point) || point.length !== 3) {
        fail('each control point must have three coordinates');
      }
      for (k = 0; k < 3; k++) {
        if (!isFiniteNumber(point[k])) {
          fail('control point coordinates must be finite');
        }
      }
    }
  }

  var size = spline.size;
  if (!isNestedArray(size) || size.length !== 2) {
    fail('spline size must have two dimensions');
  }
  if (size[0] !== n0 || size[1] !== n1) {
    fail('control net size does not match its data');
  }
  for (d = 0; d < 2; d++) {
    if (size[d] <= degree[d]) {
      fail('not enough control points for degree ' + degree[d] + ' in dimension ' + d);
    }
  }

  var weights = spline.weights;
  if (weights !== undefined && weights !== null) {
    if (!isNestedArray(weights) || weights.length !== n0) {
      fail('weights must be a nested array matching the control net');
    }
    for (i = 0; i < n0; i++) {
      if (!isNestedArray(weights[i]) || weights[i].length !== n1) {
        fail('weights must be a nested array matching the control net');
      }
      for (j = 0; j < n1; j++) {
        var w = weights[i][j];
        if (!isFiniteNumber(w) || !(w > 0)) {
          fail('weights must be finite and positive');
        }
      }
    }
  }

  var boundary = spline.boundary;
  if (!isNestedArray(boundary) || boundary.length !== 2) {
    fail('boundary must be specified for both dimensions');
  }
  var closedCount = 0;
  for (d = 0; d < 2; d++) {
    if (boundary[d] === 'closed') {
      closedCount++;
    } else if (boundary[d] !== 'clamped') {
      fail('non-periodic dimensions must be clamped');
    }
  }
  if (closedCount > 1) {
    fail('at most one dimension may be periodic');
  }

  var axes = [analyzeAxis(spline, 0, n0), analyzeAxis(spline, 1, n1)];

  return {
    n0: n0,
    n1: n1,
    points: points,
    weights: weights,
    boundary: boundary,
    closed: [boundary[0] === 'closed', boundary[1] === 'closed'],
    axes: axes
  };
}

function analyzeAxis (spline, dimension, size) {
  var degree = spline.degree[dimension];
  var boundary = spline.boundary[dimension];
  var knots = spline.knots;
  var explicit = !!(knots && Array.isArray(knots) && Array.isArray(knots[dimension]));
  var K = null;
  var i;

  if (explicit) {
    var raw = knots[dimension];
    if (raw.length === 0) {
      fail('knot vectors must not be empty');
    }
    K = new Array(raw.length);
    for (i = 0; i < raw.length; i++) {
      if (!isFiniteNumber(raw[i])) {
        fail('knots must be finite');
      }
      if (i > 0 && raw[i] < raw[i - 1]) {
        fail('knots must be nondecreasing');
      }
      K[i] = raw[i];
    }
  }

  if (boundary === 'closed') {
    return analyzePeriodicAxis(dimension, degree, size, explicit, K);
  }
  return analyzeClampedAxis(dimension, degree, size, explicit, K);
}

function analyzePeriodicAxis (dimension, degree, size, explicit, K) {
  var i;
  if (explicit && K.length !== size + 1) {
    fail('expected ' + (size + 1) + ' knots for a periodic dimension but got ' + K.length);
  }

  var domainStart = explicit ? K[0] : 0;
  var domainEnd = explicit ? K[size] : size;
  var period = domainEnd - domainStart;
  if (!(period > 0) || !isFiniteNumber(period)) {
    fail('periodic dimension must have a positive finite period');
  }

  function knot (j) {
    return explicit ? K[j] : j;
  }

  var intervals = [];
  for (i = 0; i < size; i++) {
    var a = knot(i);
    var b = knot(i + 1);
    if (b < a) fail('knots must be nondecreasing');
    if (b === a) continue;
    if (b - a < period * MIN_RELATIVE_INTERVAL) {
      fail('non-zero knot interval is too narrow');
    }
    intervals.push([a, b]);
  }
  if (intervals.length === 0) fail('periodic dimension has no non-zero knot interval');

  if (explicit) {
    var counts = Object.create(null);
    for (i = 1; i < size; i++) {
      var value = K[i];
      if (value <= K[0] || value >= K[size]) continue;
      var key = String(value);
      counts[key] = (counts[key] || 0) + 1;
      if (counts[key] > degree - 1) {
        fail('interior knot multiplicity must not exceed degree - 1');
      }
    }
  }

  return {
    dimension: dimension,
    degree: degree,
    size: size,
    closed: true,
    knot: knot,
    domain: [domainStart, domainEnd],
    intervals: intervals
  };
}

function analyzeClampedAxis (dimension, degree, size, explicit, K) {
  var i;

  if (explicit) {
    if (K.length !== size + degree + 1) {
      fail('expected ' + (size + degree + 1) + ' knots for a clamped dimension but got ' + K.length);
    }
    var startRepeats = 0;
    var endRepeats = 0;
    for (i = 0; i < K.length; i++) {
      if (K[i] === K[0]) startRepeats++;
      if (K[i] === K[K.length - 1]) endRepeats++;
    }
    if (startRepeats !== degree + 1 || endRepeats !== degree + 1) {
      fail('knot vector is not exactly clamped at both ends');
    }

    var counts = Object.create(null);
    for (i = degree + 1; i < size; i++) {
      var value = K[i];
      if (!(value > K[degree] && value < K[size])) continue;
      var key = String(value);
      counts[key] = (counts[key] || 0) + 1;
      if (counts[key] > degree - 1) {
        fail('interior knot multiplicity must not exceed degree - 1');
      }
    }
  }

  function knot (j) {
    if (explicit) return K[j];
    if (j < degree) return degree;
    if (j > size) return size;
    return j;
  }

  var lo = explicit ? K[degree] : degree;
  var hi = explicit ? K[size] : size;
  var total = hi - lo;
  if (!(total > 0) || !isFiniteNumber(total)) {
    fail('dimension must have a positive finite domain');
  }

  var intervals = [];
  for (i = degree; i < size; i++) {
    var a = knot(i);
    var b = knot(i + 1);
    if (b < a) fail('knots must be nondecreasing');
    if (b === a) continue;
    if (b - a < total * MIN_RELATIVE_INTERVAL) {
      fail('non-zero knot interval is too narrow');
    }
    intervals.push([a, b]);
  }
  if (intervals.length === 0) fail('dimension has no non-zero knot interval');

  return {
    dimension: dimension,
    degree: degree,
    size: size,
    closed: false,
    knot: knot,
    domain: [lo, hi],
    intervals: intervals
  };
}

function makeStations (axis, resolution) {
  var stations = [axis.domain[0]];
  for (var i = 0; i < axis.intervals.length; i++) {
    var a = axis.intervals[i][0];
    var b = axis.intervals[i][1];
    for (var s = 1; s <= resolution; s++) {
      // Emit the exact knot value at the end of an interval so shared knots and
      // the domain endpoint appear exactly once, without accumulated rounding.
      stations.push(s === resolution ? b : a + (b - a) * (s / resolution));
    }
  }
  if (stations.length < 2) fail('not enough sample stations');
  if (stations[stations.length - 1] !== axis.domain[1]) {
    fail('sample stations do not cover the domain');
  }
  for (var k = 1; k < stations.length; k++) {
    if (!(stations[k] > stations[k - 1])) {
      fail('sample stations must be strictly increasing');
    }
  }
  return stations;
}

function validateOptions (options, structure) {
  if (!options || typeof options !== 'object') {
    fail('options with a resolution are required');
  }

  var resolution = options.resolution;
  if (!Array.isArray(resolution) || resolution.length !== 2) {
    fail('resolution must be an array [nu, nv]');
  }
  for (var d = 0; d < 2; d++) {
    if (!isInteger(resolution[d]) || resolution[d] < MIN_RESOLUTION || resolution[d] > MAX_RESOLUTION) {
      fail('resolution values must be integers between ' + MIN_RESOLUTION + ' and ' + MAX_RESOLUTION);
    }
  }

  var collapsed = options.collapsed;
  if (collapsed === undefined || collapsed === null) {
    collapsed = [];
  }
  if (!Array.isArray(collapsed)) {
    fail('collapsed must be an array of edge names');
  }
  var seen = Object.create(null);
  for (var i = 0; i < collapsed.length; i++) {
    var name = collapsed[i];
    if (typeof name !== 'string' || EDGE_NAMES.indexOf(name) === -1) {
      fail('unknown collapsed edge ' + String(name));
    }
    if (seen[name]) {
      fail('duplicate collapsed edge ' + name);
    }
    seen[name] = true;
  }

  var edges = {
    umin: !!seen.umin,
    umax: !!seen.umax,
    vmin: !!seen.vmin,
    vmax: !!seen.vmax
  };

  if ((edges.umin || edges.umax) && structure.closed[0]) {
    fail('collapsed edges are not allowed on a periodic dimension');
  }
  if ((edges.vmin || edges.vmax) && structure.closed[1]) {
    fail('collapsed edges are not allowed on a periodic dimension');
  }
  // Adjacent edges share a corner, so a single cell would collapse in two
  // independent directions; only opposite pairs are supported.
  if ((edges.umin || edges.umax) && (edges.vmin || edges.vmax)) {
    fail('adjacent collapsed edges are not supported');
  }

  validateCollapsedRows(structure, edges);

  return { resolution: resolution, collapsed: edges };
}

function validateCollapsedRows (structure, edges) {
  var points = structure.points;
  var n0 = structure.n0;
  var n1 = structure.n1;

  function identicalRow (accessor, length, name) {
    var reference = accessor(0);
    for (var t = 1; t < length; t++) {
      var point = accessor(t);
      for (var d = 0; d < 3; d++) {
        if (point[d] !== reference[d]) {
          fail('control points on the collapsed ' + name + ' edge are not identical');
        }
      }
    }
  }

  if (edges.umin) identicalRow(function (j) { return points[0][j]; }, n1, 'umin');
  if (edges.umax) identicalRow(function (j) { return points[n0 - 1][j]; }, n1, 'umax');
  if (edges.vmin) identicalRow(function (i) { return points[i][0]; }, n0, 'vmin');
  if (edges.vmax) identicalRow(function (i) { return points[i][n1 - 1]; }, n0, 'vmax');
}

function sampleCad (spline, options) {
  var structure = validateStructure(spline);
  var settings = validateOptions(options, structure);

  var resolution = settings.resolution;
  var collapsed = settings.collapsed;

  var rotations = !(spline.weights === undefined || spline.weights === null);
  var tangents = {
    u: rotations ? bindAnalytic(spline, [1, 0]) : spline.evaluator([1, 0]),
    v: rotations ? bindAnalytic(spline, [0, 1]) : spline.evaluator([0, 1]),
    uv: rotations ? bindAnalytic(spline, [1, 1]) : spline.evaluator([1, 1])
  };

  var uAxis = structure.axes[0];
  var vAxis = structure.axes[1];
  var uStations = makeStations(uAxis, resolution[0]);
  var vStations = makeStations(vAxis, resolution[1]);
  var nu = uStations.length;
  var nv = vStations.length;

  var uClosed = structure.closed[0];
  var vClosed = structure.closed[1];

  var positions = [];
  var indexByKey = new Map();
  var gridIndex = new Array(nu * nv);

  // Position identity: declared pole rows collapse to one key, periodic first
  // and last stations share one key, everything else keeps its (u, v) identity.
  // Positions are never welded by spatial proximity.
  function positionKey (i, j) {
    if (collapsed.vmin && j === 0) return 'vmin';
    if (collapsed.vmax && j === nv - 1) return 'vmax';
    if (collapsed.umin && i === 0) return 'umin';
    if (collapsed.umax && i === nu - 1) return 'umax';
    var ci = (uClosed && i === nu - 1) ? 0 : i;
    var cj = (vClosed && j === nv - 1) ? 0 : j;
    return 'g' + ci + ',' + cj;
  }

  var scratch = [0, 0, 0];
  var i, j;
  for (j = 0; j < nv; j++) {
    for (i = 0; i < nu; i++) {
      var key = positionKey(i, j);
      var index = indexByKey.get(key);
      if (index === undefined) {
        spline.evaluate(scratch, uStations[i], vStations[j]);
        if (!isFiniteNumber(scratch[0]) || !isFiniteNumber(scratch[1]) || !isFiniteNumber(scratch[2])) {
          fail('non-finite surface position');
        }
        index = positions.length / 3;
        positions.push(scratch[0], scratch[1], scratch[2]);
        indexByKey.set(key, index);
      }
      gridIndex[i + nu * j] = index;
    }
  }

  var tmpA = [0, 0, 0];
  var tmpB = [0, 0, 0];

  function ordinaryNormal (u, v) {
    tangents.u(tmpA, u, v);
    tangents.v(tmpB, u, v);
    return normalizeChecked(cross(tmpA, tmpB), 'surface tangent cross product');
  }

  // Analytic one-sided limit of Pu x Pv.  Positive sign for the min edge,
  // negative for the max edge, matching v - v0 / u - u0 -> 0+ inside.
  function poleNormal (edge, u, v) {
    var c;
    if (edge === 'vmin' || edge === 'vmax') {
      tangents.uv(tmpA, u, v);
      tangents.v(tmpB, u, v);
      c = cross(tmpA, tmpB);
      if (edge === 'vmax') { c[0] = -c[0]; c[1] = -c[1]; c[2] = -c[2]; }
    } else {
      tangents.u(tmpA, u, v);
      tangents.uv(tmpB, u, v);
      c = cross(tmpA, tmpB);
      if (edge === 'umax') { c[0] = -c[0]; c[1] = -c[1]; c[2] = -c[2]; }
    }
    return normalizeChecked(c, edge + ' pole limit normal');
  }

  function cornerUV (ci, cj, cellI, cellJ) {
    var edge = null;
    if (collapsed.vmin && cj === 0) edge = 'vmin';
    else if (collapsed.vmax && cj === nv - 1) edge = 'vmax';
    else if (collapsed.umin && ci === 0) edge = 'umin';
    else if (collapsed.umax && ci === nu - 1) edge = 'umax';

    // A pole corner uses the midpoint of the fan cell's along-edge interval and
    // the boundary for the lateral coordinate; its position stays the pole.
    if (edge === 'vmin' || edge === 'vmax') {
      return {
        edge: edge,
        u: (uStations[cellI] + uStations[cellI + 1]) * 0.5,
        v: edge === 'vmin' ? vStations[0] : vStations[nv - 1]
      };
    }
    if (edge === 'umin' || edge === 'umax') {
      return {
        edge: edge,
        u: edge === 'umin' ? uStations[0] : uStations[nu - 1],
        v: (vStations[cellJ] + vStations[cellJ + 1]) * 0.5
      };
    }
    return { edge: null, u: uStations[ci], v: vStations[cj] };
  }

  var cells = [];
  var cornerUVs = [];
  var cornerNormals = [];

  function triangleAreaSquared (ia, ib, ic) {
    var ax = positions[3 * ia], ay = positions[3 * ia + 1], az = positions[3 * ia + 2];
    var bx = positions[3 * ib] - ax, by = positions[3 * ib + 1] - ay, bz = positions[3 * ib + 2] - az;
    var cx = positions[3 * ic] - ax, cy = positions[3 * ic + 1] - ay, cz = positions[3 * ic + 2] - az;
    var nx = by * cz - bz * cy;
    var ny = bz * cx - bx * cz;
    var nz = bx * cy - by * cx;
    var area2 = nx * nx + ny * ny + nz * nz;
    if (!isFiniteNumber(area2)) fail('non-finite triangle area');
    return area2;
  }

  function emitTriangle (indices, corners) {
    if (indices[0] === indices[1] || indices[1] === indices[2] || indices[0] === indices[2]) {
      // Degenerate by construction: the cell touches a pole or periodic seam.
      return false;
    }
    if (!(triangleAreaSquared(indices[0], indices[1], indices[2]) > 0)) {
      fail('zero-area triangle');
    }
    for (var c = 0; c < 3; c++) {
      var info = cornerUV(corners[c][0], corners[c][1], corners[c][2], corners[c][3]);
      var normal = info.edge
        ? poleNormal(info.edge, info.u, info.v)
        : ordinaryNormal(info.u, info.v);
      cornerUVs.push(info.u, info.v);
      cornerNormals.push(normal[0], normal[1], normal[2]);
    }
    cells.push(indices[0], indices[1], indices[2]);
    return true;
  }

  for (i = 0; i < nu - 1; i++) {
    for (j = 0; j < nv - 1; j++) {
      var a = gridIndex[i + nu * j];
      var b = gridIndex[(i + 1) + nu * j];
      var c = gridIndex[(i + 1) + nu * (j + 1)];
      var d = gridIndex[i + nu * (j + 1)];

      // Diagonal from lower-left (a) to upper-right (c), positive UV winding.
      // Corners carry (grid i, grid j, cell i, cell j) so pole corners can use
      // their fan cell's along-edge midpoint.  A cell touching a pole row (or
      // the periodic seam) has exactly one degenerate triangle by index
      // identity; the fan keeps only the non-degenerate one.
      var kept = emitTriangle(
        [a, b, c],
        [[i, j, i, j], [i + 1, j, i, j], [i + 1, j + 1, i, j]]
      );
      kept = emitTriangle(
        [a, c, d],
        [[i, j, i, j], [i + 1, j + 1, i, j], [i, j + 1, i, j]]
      ) || kept;

      if (!kept) {
        fail('cell produced no non-degenerate triangle');
      }
    }
  }

  if (cells.length === 0) fail('surface produced no triangles');

  return {
    positions: positions,
    cells: cells,
    cornerUVs: cornerUVs,
    cornerNormals: cornerNormals
  };
}

function bindAnalytic (spline, order) {
  return createAnalyticDerivatives(order).bind(spline);
}

module.exports = sampleCad;
