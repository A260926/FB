'use strict';

//
// Constraint solver and result lifecycle for the surface fairing plan.
//
// The displacement energy matrix A (see surface-operator.js) and the fixed
// constraint matrix B are assembled once, when the plan is created. The
// equality constrained minimizer is computed through a reduced KKT system:
//
//   minimize 1/2 d^T A d  subject to  B d = rhs
//
// Using an orthonormal basis Q of the row space of B (B = L Q), the
// constraints become Q d = c with c = L^+ rhs. Then
//
//   d = U c,   U = A^{-1} Q^T,   S = Q U,
//   mu = S^{-1} c,   d = U mu
//
// All factorizations (A, S, L^T L) are done at construction. solve() only
// forms the right hand side from the requested targets, checks that it lies
// in the range of B (otherwise the constraints conflict) and performs the
// small triangular solves.
//
// This handles duplicate and otherwise linearly dependent constraint rows
// without a penalty term: the constraints are satisfied exactly whenever they
// are consistent, and the energy minimizer is unique because A is positive
// definite (lambda > 0).
//

var nurbs = require('../../nurbs');
var operator = require('./surface-operator');

// Relative tolerance used to decide the numerical rank of B.
var RANK_TOL = 1e-10;

// Relative tolerance for deciding that a right hand side is consistent with
// the constraints.
var CONSISTENCY_TOL = 1e-8;

function fail (message) {
  throw new RangeError('createFairingPlan: ' + message);
}

function isFiniteNumber (x) {
  return typeof x === 'number' && isFinite(x);
}

function validateLambda (lambda) {
  if (lambda === undefined) fail('lambda is required');
  if (!isFiniteNumber(lambda)) fail('lambda must be finite');
  if (lambda < 1e-5 || lambda > 1) fail('lambda must lie in [1e-5, 1]');
  return lambda;
}

function validateAnchors (anchors, domain) {
  var i, k;
  if (!Array.isArray(anchors)) fail('anchors must be an array');
  if (anchors.length < 1 || anchors.length > 32) fail('anchors must contain between 1 and 32 points');
  var out = new Array(anchors.length);
  for (i = 0; i < anchors.length; i++) {
    var a = anchors[i];
    if (!Array.isArray(a) || a.length !== 2) fail('each anchor must be a [u, v] pair');
    var u = a[0];
    var v = a[1];
    if (!isFiniteNumber(u) || !isFiniteNumber(v)) fail('non-finite anchor parameter');
    if (Math.abs(u) > 10 || Math.abs(v) > 10) fail('anchor parameter out of range');
    if (u < domain[0][0] || u > domain[0][1] || v < domain[1][0] || v > domain[1][1]) {
      fail('anchor parameter outside the parameter domain');
    }
    out[i] = [u, v];
  }
  return out;
}

//
// Dense Cholesky factorization of a symmetric positive definite matrix given
// as an array of row arrays / typed arrays. Returns the lower triangular
// factor. Throws if the matrix is not positive definite.
//
function cholesky (A, n) {
  var L = new Array(n);
  var i, j, k;
  for (i = 0; i < n; i++) L[i] = new Float64Array(n);
  for (i = 0; i < n; i++) {
    for (j = 0; j <= i; j++) {
      var sum = A[i][j];
      for (k = 0; k < j; k++) sum -= L[i][k] * L[j][k];
      if (i === j) {
        if (!(sum > 0)) throw new Error('not positive definite');
        L[i][j] = Math.sqrt(sum);
      } else {
        L[i][j] = sum / L[j][j];
      }
    }
  }
  return L;
}

function choleskySolve (L, b, n) {
  var x = new Float64Array(n);
  var i, k, sum;
  for (i = 0; i < n; i++) {
    sum = b[i];
    for (k = 0; k < i; k++) sum -= L[i][k] * x[k];
    x[i] = sum / L[i][i];
  }
  for (i = n - 1; i >= 0; i--) {
    sum = x[i];
    for (k = i + 1; k < n; k++) sum -= L[k][i] * x[k];
    x[i] = sum / L[i][i];
  }
  return x;
}

//
// Orthonormal basis of the row space of B, using modified Gram-Schmidt with
// reorthogonalization. Returns { Q: rows of length n, rank }.
//
function rowSpaceBasis (B, m, n) {
  var Q = [];
  var scale = 0;
  var i, j;
  for (i = 0; i < m; i++) {
    for (j = 0; j < n; j++) {
      var abs = Math.abs(B[i][j]);
      if (abs > scale) scale = abs;
    }
  }
  var tol = RANK_TOL * Math.max(1, scale);
  for (i = 0; i < m; i++) {
    var v = new Float64Array(n);
    for (j = 0; j < n; j++) v[j] = B[i][j];
    for (var pass = 0; pass < 2; pass++) {
      for (var q = 0; q < Q.length; q++) {
        var dot = 0;
        for (j = 0; j < n; j++) dot += v[j] * Q[q][j];
        for (j = 0; j < n; j++) v[j] -= dot * Q[q][j];
      }
    }
    var nrm = 0;
    for (j = 0; j < n; j++) nrm += v[j] * v[j];
    nrm = Math.sqrt(nrm);
    if (nrm > tol) {
      for (j = 0; j < n; j++) v[j] /= nrm;
      Q.push(v);
    }
  }
  return Q;
}

//
// Build the constraint rows for a list of anchors. Each anchor contributes
// one row per ambient coordinate. Rows are stored with the same layout as
// the displacement vector: index 3 * (i * nv + j) + c.
//
function buildConstraints (model, anchors) {
  var na = anchors.length;
  var nv = model.size[1];
  var npts = model.size[0] * nv;
  var n = 3 * npts;
  var m = 3 * na;
  var B = new Array(m);
  var ref = new Array(na * 3);
  var i, k, c;

  for (i = 0; i < m; i++) B[i] = new Float64Array(n);

  for (k = 0; k < na; k++) {
    var ev = operator.evaluateAt(model, anchors[k][0], anchors[k][1]);
    for (c = 0; c < 3; c++) {
      ref[3 * k + c] = ev.S[c];
      var row = B[3 * k + c];
      for (var b = 0; b < ev.basis.length; b++) {
        var entry = ev.basis[b];
        var col = 3 * (entry.i * nv + entry.j) + c;
        row[col] += entry.value;
      }
    }
  }

  return { B: B, ref: ref, m: m, n: n };
}

//
// KKT plan construction. Returns null when a numerical factorization fails;
// solve() then reports numerical_failure.
//
function assemble (reference, anchors, lambda) {
  var n = reference.nrows;
  var A = reference.K;

  var constraints = buildConstraints(reference.model, anchors);
  var B = constraints.B;
  var m = constraints.m;

  var Q = rowSpaceBasis(B, m, n);
  var r = Q.length;

  // L = B Q^T (m x r). L^T L and L^T rhs.
  var L = new Array(m);
  var i, j, k;
  for (i = 0; i < m; i++) {
    L[i] = new Float64Array(r);
    for (j = 0; j < r; j++) {
      var sum = 0;
      for (k = 0; k < n; k++) sum += B[i][k] * Q[j][k];
      L[i][j] = sum;
    }
  }

  var LtL = new Array(r);
  for (i = 0; i < r; i++) {
    LtL[i] = new Float64Array(r);
    for (j = 0; j < r; j++) {
      var s = 0;
      for (k = 0; k < m; k++) s += L[k][i] * L[k][j];
      LtL[i][j] = s;
    }
  }
  var LtLFactor = cholesky(LtL, r);

  // U = A^{-1} Q^T (n x r).
  var AFactor = cholesky(A, n);
  var U = new Array(n);
  for (i = 0; i < n; i++) U[i] = new Float64Array(r);
  for (j = 0; j < r; j++) {
    var col = new Float64Array(n);
    for (i = 0; i < n; i++) col[i] = Q[j][i];
    var sol = choleskySolve(AFactor, col, n);
    for (i = 0; i < n; i++) U[i][j] = sol[i];
  }

  // S = Q U (r x r), symmetric positive definite.
  var S = new Array(r);
  for (i = 0; i < r; i++) {
    S[i] = new Float64Array(r);
    for (j = 0; j < r; j++) {
      var t = 0;
      for (k = 0; k < n; k++) t += Q[i][k] * U[k][j];
      S[i][j] = t;
    }
  }
  var SFactor = cholesky(S, r);

  // P = L (L^T L)^{-1} maps reduced multipliers to original row multipliers:
  // B^T P = Q^T.
  var P = new Array(m);
  for (i = 0; i < m; i++) P[i] = new Float64Array(r);
  for (i = 0; i < m; i++) {
    var e = new Float64Array(r);
    for (j = 0; j < r; j++) e[j] = L[i][j];
    var y = choleskySolve(LtLFactor, e, r);
    for (j = 0; j < r; j++) P[i][j] = y[j];
  }

  return {
    A: A,
    n: n,
    m: m,
    r: r,
    Q: Q,
    L: L,
    U: U,
    P: P,
    LtLFactor: LtLFactor,
    SFactor: SFactor,
    B: B,
    ref: constraints.ref
  };
}

function statusFailure (status) {
  return {
    status: status,
    surface: null,
    energy: null,
    constraintResidual: null,
    stationarityResidual: null
  };
}

function createFairingPlan (spline, options) {
  options = options || {};
  var lambda = validateLambda(options.lambda);

  // Validate everything before allocating or modifying anything: a rejected
  // input leaves the source untouched.
  var domain = operator.validateSurface(spline);
  var anchors = validateAnchors(options.anchors, domain);

  // Deep snapshot of the reference surface; later edits of the source data
  // cannot reach the plan.
  var reference = operator.buildReference(spline, lambda);

  var assembled = null;
  var numericalFailure = false;
  try {
    assembled = assemble(reference, anchors, lambda);
  } catch (e) {
    // A numerical factorization failure is reported through solve(), as a
    // plan level status rather than an exception.
    if (!(e instanceof Error) || /not positive definite/.test(e.message)) {
      numericalFailure = true;
    } else {
      throw e;
    }
  }

  var plan = {
    assemblyCount: 1,
    factorizationCount: 1,
    lambda: lambda
  };

  plan.solve = function solve (targets) {
    if (numericalFailure || !assembled) return statusFailure('numerical_failure');
    if (!Array.isArray(targets) || targets.length !== anchors.length) {
      fail('targets must be an array with one entry per anchor');
    }
    var i, k, c;
    var na = anchors.length;
    var rhs = new Float64Array(3 * na);
    for (k = 0; k < na; k++) {
      var t = targets[k];
      if (!Array.isArray(t) || t.length !== 3) fail('each target must be a three dimensional position');
      for (c = 0; c < 3; c++) {
        if (!isFiniteNumber(t[c])) fail('non-finite target coordinate');
        rhs[3 * k + c] = t[c] - assembled.ref[3 * k + c];
      }
    }

    var r = assembled.r;
    var m = assembled.m;
    var n = assembled.n;

    // c = (L^T L)^{-1} L^T rhs, and the residual of the least squares problem
    // tells us whether the requested targets are consistent with a single
    // displacement field.
    var Ltr = new Float64Array(r);
    var j;
    for (i = 0; i < r; i++) {
      var s = 0;
      for (k = 0; k < m; k++) s += assembled.L[k][i] * rhs[k];
      Ltr[i] = s;
    }
    var coef = choleskySolve(assembled.LtLFactor, Ltr, r);

    var consResidual = 0;
    for (k = 0; k < m; k++) {
      var pred = 0;
      for (j = 0; j < r; j++) pred += assembled.L[k][j] * coef[j];
      var diff = Math.abs(pred - rhs[k]);
      if (diff > consResidual) consResidual = diff;
    }
    var rhsScale = 1;
    for (k = 0; k < m; k++) if (Math.abs(rhs[k]) > rhsScale) rhsScale = Math.abs(rhs[k]);
    if (!(consResidual <= CONSISTENCY_TOL * rhsScale)) {
      return statusFailure('inconsistent');
    }

    // mu = S^{-1} c and d = U mu.
    var muReduced = choleskySolve(assembled.SFactor, coef, r);
    var d = new Float64Array(n);
    for (i = 0; i < n; i++) {
      var sum = 0;
      for (j = 0; j < r; j++) sum += assembled.U[i][j] * muReduced[j];
      d[i] = sum;
    }

    // Multipliers for the original rows. d = A^{-1} B^T lambda, so the
    // stationarity form A d + B^T mu vanishes with mu = -lambda.
    var muOriginal = new Float64Array(m);
    for (k = 0; k < m; k++) {
      var mu = 0;
      for (j = 0; j < r; j++) mu += assembled.P[k][j] * muReduced[j];
      muOriginal[k] = -mu;
    }

    // Residuals.
    var Ad = new Float64Array(n);
    var Btmu = new Float64Array(n);
    for (i = 0; i < n; i++) {
      var a = 0;
      for (j = 0; j < n; j++) a += assembled.A[i][j] * d[j];
      Ad[i] = a;
    }
    for (k = 0; k < m; k++) {
      if (muOriginal[k] === 0) continue;
      var row = assembled.B[k];
      for (i = 0; i < n; i++) Btmu[i] += row[i] * muOriginal[k];
    }
    var stationarity = 0;
    var AdInf = 0;
    var BtmuInf = 0;
    for (i = 0; i < n; i++) {
      var res = Math.abs(Ad[i] + Btmu[i]);
      if (res > stationarity) stationarity = res;
      if (Math.abs(Ad[i]) > AdInf) AdInf = Math.abs(Ad[i]);
      if (Math.abs(Btmu[i]) > BtmuInf) BtmuInf = Math.abs(Btmu[i]);
    }
    var denom = Math.max(1, AdInf, BtmuInf);
    var stationarityResidual = stationarity / denom;

    // Constraint residual measured directly through B.
    var maxConstraint = 0;
    for (k = 0; k < m; k++) {
      var val = 0;
      var rowB = assembled.B[k];
      for (i = 0; i < n; i++) val += rowB[i] * d[i];
      var cr = Math.abs(val - rhs[k]);
      if (cr > maxConstraint) maxConstraint = cr;
    }

    // Energy of the resulting displacement, in the exact discrete sense.
    var D = new Array(reference.model.size[0]);
    for (i = 0; i < reference.model.size[0]; i++) {
      D[i] = new Array(reference.model.size[1]);
      for (j = 0; j < reference.model.size[1]; j++) {
        var base = 3 * (i * reference.model.size[1] + j);
        D[i][j] = [d[base], d[base + 1], d[base + 2]];
      }
    }
    var energy = operator.energyAt(reference.model, reference.quad, D, lambda);

    if (!isFinite(energy) || !isFinite(maxConstraint) || !isFinite(stationarityResidual)) {
      return statusFailure('numerical_failure');
    }

    // Independent output surface built through the original constructor.
    var points = new Array(reference.model.size[0]);
    for (i = 0; i < reference.model.size[0]; i++) {
      points[i] = new Array(reference.model.size[1]);
      for (j = 0; j < reference.model.size[1]; j++) {
        var p = reference.model.points[i][j];
        var dd = D[i][j];
        points[i][j] = [p[0] + dd[0], p[1] + dd[1], p[2] + dd[2]];
      }
    }

    var surface = nurbs({
      points: points,
      weights: operator.cloneData(reference.model.weights),
      degree: reference.model.degree.slice(),
      knots: [reference.model.knots[0].slice(), reference.model.knots[1].slice()],
      boundary: reference.model.boundary.slice()
    });

    return {
      status: 'ok',
      surface: surface,
      energy: energy,
      constraintResidual: maxConstraint,
      stationarityResidual: stationarityResidual
    };
  };

  return plan;
}

module.exports = createFairingPlan;
