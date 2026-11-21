'use strict';

//
// Tests for extras/fairing.js.
//
// These tests are independent of the production implementation: they
// re-derive the rational basis and its derivatives from the knot vectors,
// integrate with their own 6x6 Gauss-Legendre product rule, and solve small
// dense KKT systems with their own Gaussian elimination. Nothing here reads
// the new modules' internals or trusts the reported residuals as proof.
//

// ---------------------------------------------------------------------------
// Small generic helpers
// ---------------------------------------------------------------------------

function copy (x) {
  if (Array.isArray(x)) return x.map(copy);
  if (x && typeof x === 'object') {
    const out = {};
    for (const k of Object.keys(x)) out[k] = copy(x[k]);
    return out;
  }
  return x;
}

function maxAbsDiff (a, b) {
  let m = 0;
  function walk (x, y) {
    if (Array.isArray(x)) {
      if (!Array.isArray(y) || y.length !== x.length) { m = Infinity; return; }
      for (let i = 0; i < x.length; i++) walk(x[i], y[i]);
    } else {
      m = Math.max(m, Math.abs(x - y));
    }
  }
  walk(a, b);
  return m;
}

function norm (v) {
  return Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The same standard 6 point Gauss-Legendre nodes/weights on [-1, 1].
const GL_X = [
  -0.9324695142031521, -0.6612093864662645, -0.2386191860831969,
  0.2386191860831969, 0.6612093864662645, 0.9324695142031521
];
const GL_W = [
  0.17132449237917036, 0.3607615730481386, 0.46791393457269104,
  0.46791393457269104, 0.3607615730481386, 0.17132449237917036
];

// ---------------------------------------------------------------------------
// Independent B-spline / NURBS basis, value and first two derivatives
// ---------------------------------------------------------------------------

function basisCount (knots, p) {
  return knots.length - p - 1;
}

function basisValue (i, p, u, knots) {
  if (p === 0) {
    // Half open intervals; quadrature points never hit interior knots.
    return (u >= knots[i] && u < knots[i + 1]) ? 1 : 0;
  }
  let a = 0;
  let b = 0;
  const d1 = knots[i + p] - knots[i];
  if (d1 > 0) a = (u - knots[i]) / d1 * basisValue(i, p - 1, u, knots);
  const d2 = knots[i + p + 1] - knots[i + 1];
  if (d2 > 0) b = (knots[i + p + 1] - u) / d2 * basisValue(i + 1, p - 1, u, knots);
  return a + b;
}

// Derivative of order 0, 1 or 2 by the derivative recursion
//   N'_{i,p} = p/(t_{i+p}-t_i) N_{i,p-1}
//            - p/(t_{i+p+1}-t_{i+1}) N_{i+1,p-1}.
function basisDerivative (i, p, u, knots, order) {
  if (order === 0) return basisValue(i, p, u, knots);
  if (p === 0) return 0;
  let s = 0;
  const d1 = knots[i + p] - knots[i];
  if (d1 > 0) s += p / d1 * basisDerivative(i, p - 1, u, knots, order - 1);
  const d2 = knots[i + p + 1] - knots[i + 1];
  if (d2 > 0) s -= p / d2 * basisDerivative(i + 1, p - 1, u, knots, order - 1);
  return s;
}

function axisBasis (knots, p, u) {
  const n = basisCount(knots, p);
  const N = new Array(n);
  const dN = new Array(n);
  const d2N = new Array(n);
  for (let i = 0; i < n; i++) {
    N[i] = basisValue(i, p, u, knots);
    dN[i] = basisDerivative(i, p, u, knots, 1);
    d2N[i] = basisDerivative(i, p, u, knots, 2);
  }
  return { N, dN, d2N };
}

function quotient (A, Au, Av, Auu, Auv, Avv, W, Wu, Wv, Wuu, Wuv, Wvv) {
  const n = A.length;
  const S = new Array(n);
  const Su = new Array(n);
  const Sv = new Array(n);
  const Suu = new Array(n);
  const Suv = new Array(n);
  const Svv = new Array(n);
  for (let c = 0; c < n; c++) {
    S[c] = A[c] / W;
    Su[c] = (Au[c] - S[c] * Wu) / W;
    Sv[c] = (Av[c] - S[c] * Wv) / W;
    Suu[c] = (Auu[c] - 2 * Su[c] * Wu - S[c] * Wuu) / W;
    Suv[c] = (Auv[c] - Su[c] * Wv - Sv[c] * Wu - S[c] * Wuv) / W;
    Svv[c] = (Avv[c] - 2 * Sv[c] * Wv - S[c] * Wvv) / W;
  }
  return { S, Su, Sv, Suu, Suv, Svv };
}

// Rational surface evaluation: point, first and second partials, and the
// rational basis functions with their derivatives.
function evaluateRational (surface, u, v) {
  const ku = surface.knots[0];
  const kv = surface.knots[1];
  const pu = surface.degree[0];
  const pv = surface.degree[1];
  const nu = basisCount(ku, pu);
  const nv = basisCount(kv, pv);
  const bu = axisBasis(ku, pu, u);
  const bv = axisBasis(kv, pv, v);
  // Non-rational surfaces have no weights: use unit weights.
  const weights = surface.weights || surface.points.map((row) => row.map(() => 1));

  const acc = {
    W: 0, Wu: 0, Wv: 0, Wuu: 0, Wuv: 0, Wvv: 0,
    A: [0, 0, 0], Au: [0, 0, 0], Av: [0, 0, 0],
    Auu: [0, 0, 0], Auv: [0, 0, 0], Avv: [0, 0, 0]
  };
  const basis = [];
  for (let i = 0; i < nu; i++) {
    for (let j = 0; j < nv; j++) {
      const w = weights[i][j];
      const P = surface.points[i][j];
      const val = bu.N[i] * bv.N[j];
      const du = bu.dN[i] * bv.N[j];
      const dv = bu.N[i] * bv.dN[j];
      const duu = bu.d2N[i] * bv.N[j];
      const duv = bu.dN[i] * bv.dN[j];
      const dvv = bu.N[i] * bv.d2N[j];
      acc.W += w * val;
      acc.Wu += w * du;
      acc.Wv += w * dv;
      acc.Wuu += w * duu;
      acc.Wuv += w * duv;
      acc.Wvv += w * dvv;
      for (let c = 0; c < 3; c++) {
        acc.A[c] += w * P[c] * val;
        acc.Au[c] += w * P[c] * du;
        acc.Av[c] += w * P[c] * dv;
        acc.Auu[c] += w * P[c] * duu;
        acc.Auv[c] += w * P[c] * duv;
        acc.Avv[c] += w * P[c] * dvv;
      }
      basis.push({ i, j, w, Ni: bu.N[i], Nj: bv.N[j], du: bu.dN[i] * bv.N[j], dj: bu.N[i] * bv.dN[j], duu, duv, dvv });
    }
  }
  if (!(acc.W > 0)) throw new Error('bad weights in test surface');

  const S = quotient(acc.A, acc.Au, acc.Av, acc.Auu, acc.Auv, acc.Avv,
    acc.W, acc.Wu, acc.Wv, acc.Wuu, acc.Wuv, acc.Wvv);

  const rbf = [];
  for (let k = 0; k < basis.length; k++) {
    const b = basis[k];
    const val = b.Ni * b.Nj;
    const n = [b.w * val];
    const nu1 = [b.w * b.du];
    const nv1 = [b.w * b.dj];
    const nuu = [b.w * b.duu];
    const nuv = [b.w * b.duv];
    const nvv = [b.w * b.dvv];
    const q = quotient(n, nu1, nv1, nuu, nuv, nvv,
      acc.W, acc.Wu, acc.Wv, acc.Wuu, acc.Wuv, acc.Wvv);
    rbf.push({ i: b.i, j: b.j, value: q.S[0], du: q.Su[0], dv: q.Sv[0], duu: q.Suu[0], duv: q.Suv[0], dvv: q.Svv[0] });
  }

  return { S: S.S, Su: S.Su, Sv: S.Sv, Suu: S.Suu, Suv: S.Suv, Svv: S.Svv, basis: rbf };
}

// ---------------------------------------------------------------------------
// Quadrature and independent energy / matrix assembly
// ---------------------------------------------------------------------------

function quadrature (surface) {
  const ku = surface.knots[0];
  const kv = surface.knots[1];
  const pu = surface.degree[0];
  const pv = surface.degree[1];
  const nu = surface.size[0];
  const nv = surface.size[1];
  const us = [ku[pu]];
  for (let i = pu + 1; i < nu; i++) us.push(ku[i]);
  us.push(ku[nu]);
  const vs = [kv[pv]];
  for (let i = pv + 1; i < nv; i++) vs.push(kv[i]);
  vs.push(kv[nv]);

  const pts = [];
  for (let a = 0; a + 1 < us.length; a++) {
    const u0 = us[a]; const u1 = us[a + 1];
    for (let b = 0; b + 1 < vs.length; b++) {
      const v0 = vs[b]; const v1 = vs[b + 1];
      const uh = 0.5 * (u1 - u0); const um = 0.5 * (u1 + u0);
      const vh = 0.5 * (v1 - v0); const vm = 0.5 * (v1 + v0);
      for (let p = 0; p < 6; p++) {
        for (let q = 0; q < 6; q++) {
          pts.push({ u: um + uh * GL_X[p], v: vm + vh * GL_X[q], w: GL_W[p] * GL_W[q] * uh * vh });
        }
      }
    }
  }
  return pts;
}

function dot (a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

function metricAtEval (ev) {
  const n = [
    ev.Su[1] * ev.Sv[2] - ev.Su[2] * ev.Sv[1],
    ev.Su[2] * ev.Sv[0] - ev.Su[0] * ev.Sv[2],
    ev.Su[0] * ev.Sv[1] - ev.Su[1] * ev.Sv[0]
  ];
  const area = norm(n);
  const g11 = dot(ev.Su, ev.Su);
  const g12 = dot(ev.Su, ev.Sv);
  const g22 = dot(ev.Sv, ev.Sv);
  const det = g11 * g22 - g12 * g12;
  const gi = [[g22 / det, -g12 / det], [-g12 / det, g11 / det]];
  const Svec = [ev.Su, ev.Sv];
  const Ss = [ev.Suu, ev.Suv, ev.Svv];
  const Gamma = [[[0, 0], [0, 0]], [[0, 0], [0, 0]]];
  for (let a = 0; a < 2; a++) {
    for (let b = 0; b < 2; b++) {
      const d1 = dot(Svec[0], Ss[a + b]);
      const d2 = dot(Svec[1], Ss[a + b]);
      Gamma[0][a][b] = gi[0][0] * d1 + gi[0][1] * d2;
      Gamma[1][a][b] = gi[1][0] * d1 + gi[1][1] * d2;
    }
  }
  return { area, gi, Gamma };
}

// Energy recomputation from a control point displacement grid. Options allow
// dropping the Christoffel terms or the two mixed second derivative terms,
// which are used to show that they really take part in the value.
function energyFromDisplacement (surface, lambda, D, options) {
  options = options || {};
  const useGamma = options.gamma !== false;
  const useMixed = options.mixed !== false;
  const pts = quadrature(surface);
  let total = 0;
  let mass = 0;
  let bend = 0;
  for (const p of pts) {
    const ev = evaluateRational(surface, p.u, p.v);
    const m = metricAtEval(ev);
    const d0 = [0, 0, 0];
    const du = [0, 0, 0];
    const dv = [0, 0, 0];
    const duu = [0, 0, 0];
    const duv = [0, 0, 0];
    const dvv = [0, 0, 0];
    for (const b of ev.basis) {
      const d = D[b.i][b.j];
      for (let c = 0; c < 3; c++) {
        d0[c] += b.value * d[c];
        du[c] += b.du * d[c];
        dv[c] += b.dv * d[c];
        duu[c] += b.duu * d[c];
        duv[c] += b.duv * d[c];
        dvv[c] += b.dvv * d[c];
      }
    }
    const H = [[[0, 0, 0], [0, 0, 0]], [[0, 0, 0], [0, 0, 0]]];
    const second = [[duu, duv], [duv, dvv]];
    const first = [du, dv];
    for (let a = 0; a < 2; a++) {
      for (let b = 0; b < 2; b++) {
        for (let c = 0; c < 3; c++) {
          const corr = useGamma
            ? (m.Gamma[0][a][b] * first[0][c] + m.Gamma[1][a][b] * first[1][c])
            : 0;
          H[a][b][c] = second[a][b][c] - corr;
        }
      }
    }
    if (!useMixed) {
      H[0][1] = [0, 0, 0];
      H[1][0] = [0, 0, 0];
    }
    let local = 0;
    for (let a = 0; a < 2; a++) {
      for (let b = 0; b < 2; b++) {
        for (let c = 0; c < 2; c++) {
          for (let d = 0; d < 2; d++) {
            local += m.gi[a][c] * m.gi[b][d] * dot(H[a][b], H[c][d]);
          }
        }
      }
    }
    const wgt = p.w * m.area;
    bend += wgt * local;
    mass += wgt * dot(d0, d0);
    total += wgt * (local + lambda * dot(d0, d0));
  }
  return { energy: total, bend, mass, weightedMass: lambda * mass };
}

// Independent displacement energy matrix A with E(d) = d^T A d, matching the
// definition above. Only used for the dense KKT cross checks.
function assembleIndependent (surface, lambda) {
  const nv = surface.size[1];
  const npts = surface.size[0] * nv;
  const n = 3 * npts;
  const A = [];
  for (let i = 0; i < n; i++) A.push(new Float64Array(n));
  const pts = quadrature(surface);
  for (const p of pts) {
    const ev = evaluateRational(surface, p.u, p.v);
    const m = metricAtEval(ev);
    const wq = p.w * m.area;
    const Hc = ev.basis.map((b) => {
      const R = [b.du, b.dv];
      const Rab = [b.duu, b.duv, b.duv, b.dvv];
      const out = [0, 0, 0, 0];
      for (let a = 0; a < 2; a++) {
        for (let bb = 0; bb < 2; bb++) {
          out[a * 2 + bb] = Rab[a * 2 + bb] -
            (m.Gamma[0][a][bb] * R[0] + m.Gamma[1][a][bb] * R[1]);
        }
      }
      return out;
    });
    for (let k = 0; k < ev.basis.length; k++) {
      const gk = ev.basis[k];
      const rowk = gk.i * nv + gk.j;
      for (let l = 0; l < ev.basis.length; l++) {
        const gl = ev.basis[l];
        const rowl = gl.i * nv + gl.j;
        let local = 0;
        for (let a = 0; a < 2; a++) {
          for (let b = 0; b < 2; b++) {
            for (let c = 0; c < 2; c++) {
              for (let d = 0; d < 2; d++) {
                local += m.gi[a][c] * m.gi[b][d] * Hc[k][a * 2 + b] * Hc[l][c * 2 + d];
              }
            }
          }
        }
        local += lambda * gk.value * gl.value;
        const val = wq * local;
        for (let c = 0; c < 3; c++) A[3 * rowk + c][3 * rowl + c] += val;
      }
    }
  }
  return { A, n, npts };
}

// ---------------------------------------------------------------------------
// Dense constraint rows and Gaussian elimination
// ---------------------------------------------------------------------------

function constraintRows (surface, anchors) {
  const nv = surface.size[1];
  const n = 3 * surface.size[0] * nv;
  const rows = [];
  const ref = [];
  for (const a of anchors) {
    const ev = evaluateRational(surface, a[0], a[1]);
    for (let c = 0; c < 3; c++) {
      const row = new Float64Array(n);
      for (const b of ev.basis) row[3 * (b.i * nv + b.j) + c] += b.value;
      rows.push(row);
      ref.push(ev.S[c]);
    }
  }
  return { rows, ref };
}

function gaussSolve (M, b) {
  const n = b.length;
  const A = M.map((row, i) => {
    const out = new Float64Array(n + 1);
    for (let j = 0; j < n; j++) out[j] = row[j];
    out[n] = b[i];
    return out;
  });
  for (let col = 0; col < n; col++) {
    let pivot = col;
    for (let r = col + 1; r < n; r++) {
      if (Math.abs(A[r][col]) > Math.abs(A[pivot][col])) pivot = r;
    }
    if (Math.abs(A[pivot][col]) < 1e-13) return null;
    if (pivot !== col) { const tmp = A[pivot]; A[pivot] = A[col]; A[col] = tmp; }
    const p = A[col][col];
    for (let r = col + 1; r < n; r++) {
      const f = A[r][col] / p;
      if (f === 0) continue;
      for (let c = col; c <= n; c++) A[r][c] -= f * A[col][c];
    }
  }
  const x = new Float64Array(n);
  for (let i = n - 1; i >= 0; i--) {
    let s = A[i][n];
    for (let j = i + 1; j < n; j++) s -= A[i][j] * x[j];
    if (Math.abs(A[i][i]) < 1e-13) return null;
    x[i] = s / A[i][i];
  }
  return x;
}

function independentRowBasis (rows) {
  const basis = [];
  const scale = rows.reduce((s, row) => {
    let m = 0;
    for (let j = 0; j < row.length; j++) m = Math.max(m, Math.abs(row[j]));
    return Math.max(s, m);
  }, 0) || 1;
  const tol = 1e-10 * scale;
  for (let k = 0; k < rows.length; k++) {
    const v = Float64Array.from(rows[k]);
    for (let pass = 0; pass < 2; pass++) {
      for (const q of basis) {
        let d = 0;
        for (let j = 0; j < v.length; j++) d += v[j] * q[j];
        for (let j = 0; j < v.length; j++) v[j] -= d * q[j];
      }
    }
    let nrm = 0;
    for (let j = 0; j < v.length; j++) nrm += v[j] * v[j];
    nrm = Math.sqrt(nrm);
    if (nrm > tol) {
      for (let j = 0; j < v.length; j++) v[j] /= nrm;
      basis.push(v);
    }
  }
  return basis.length;
}

// Independent minimal energy solution through a dense KKT system built from
// an independent set of constraint rows. Returns { d, energy } or null.
function independentSolution (surface, lambda, anchors, targets) {
  const { A, n } = assembleIndependent(surface, lambda);
  const { rows, ref } = constraintRows(surface, anchors);
  const m = rows.length;
  const rhs = new Float64Array(m);
  for (let k = 0; k < m; k++) {
    const anchorIndex = Math.floor(k / 3);
    rhs[k] = targets[anchorIndex][k % 3] - ref[k];
  }

  // Select an independent subset by rank. Keep the corresponding rows and rhs.
  const selected = [];
  const { rows: chosen } = (function () {
    const basis = [];
    const out = [];
    const idx = [];
    const scale = rows.reduce((s, row) => {
      let mx = 0;
      for (let j = 0; j < row.length; j++) mx = Math.max(mx, Math.abs(row[j]));
      return Math.max(s, mx);
    }, 0) || 1;
    const tol = 1e-10 * scale;
    for (let k = 0; k < m; k++) {
      const v = Float64Array.from(rows[k]);
      for (let pass = 0; pass < 2; pass++) {
        for (const q of basis) {
          let d = 0;
          for (let j = 0; j < v.length; j++) d += v[j] * q[j];
          for (let j = 0; j < v.length; j++) v[j] -= d * q[j];
        }
      }
      let nrm = 0;
      for (let j = 0; j < v.length; j++) nrm += v[j] * v[j];
      nrm = Math.sqrt(nrm);
      if (nrm > tol) {
        for (let j = 0; j < v.length; j++) v[j] /= nrm;
        basis.push(v);
        out.push(rows[k]);
        idx.push(k);
      }
    }
    selected.push(...idx);
    return { rows: out };
  })();

  const r = chosen.length;
  const size = n + r;
  const K = [];
  for (let i = 0; i < size; i++) K.push(new Float64Array(size));
  const bvec = new Float64Array(size);
  for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) K[i][j] = A[i][j];
  }
  for (let a = 0; a < r; a++) {
    for (let j = 0; j < n; j++) {
      K[n + a][j] = chosen[a][j];
      K[j][n + a] = chosen[a][j];
    }
    bvec[n + a] = rhs[selected[a]];
  }
  const sol = gaussSolve(K, bvec);
  if (!sol) return null;

  // Confirm the dependent rows were consistent; return null otherwise.
  let worst = 0;
  for (let k = 0; k < m; k++) {
    let val = 0;
    for (let j = 0; j < n; j++) val += rows[k][j] * sol[j];
    worst = Math.max(worst, Math.abs(val - rhs[k]));
  }
  if (worst > 1e-8) return null;

  const d = Array.from(sol.slice(0, n));
  let energy = 0;
  for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) energy += d[i] * A[i][j] * d[j];
  return { d, energy, dependentRows: rows.length - r };
}

// ---------------------------------------------------------------------------
// Surface construction and conversion helpers
// ---------------------------------------------------------------------------

function makeSurface (api, spec) {
  return api.nurbs({
    points: copy(spec.points),
    weights: copy(spec.weights),
    degree: spec.degree.slice(),
    knots: [spec.knots[0].slice(), spec.knots[1].slice()],
    boundary: ['clamped', 'clamped']
  });
}

function displacementGrid (surface, reference, size) {
  const D = [];
  for (let i = 0; i < size[0]; i++) {
    D[i] = [];
    for (let j = 0; j < size[1]; j++) {
      D[i][j] = [
        surface.points[i][j][0] - reference.points[i][j][0],
        surface.points[i][j][1] - reference.points[i][j][1],
        surface.points[i][j][2] - reference.points[i][j][2]
      ];
    }
  }
  return D;
}

function surfacePoint (surface, u, v) {
  return evaluateRational(surface, u, v).S;
}

function checkMetadata (check, result, reference) {
  const s = result.surface;
  check(s !== reference, 'result surface must be an independent object');
  check(JSON.stringify(s.degree) === JSON.stringify(reference.degree), 'degree preserved');
  check(JSON.stringify(s.knots) === JSON.stringify(reference.knots), 'knots preserved');
  check(JSON.stringify(s.boundary) === JSON.stringify(reference.boundary), 'boundary preserved');
  check(maxAbsDiff(s.weights, reference.weights) < 1e-14, 'weights preserved');
  check(s.size[0] === reference.size[0] && s.size[1] === reference.size[1], 'control hull size preserved');
}

// A non-planar, positive weight, non-uniform rational surface used in several
// groups.
function curvedSpec () {
  const points = [];
  const weights = [];
  const size = 5;
  const knots = [
    [0, 0, 0, 0.9, 1.7, 2.6, 2.6, 2.6],
    [0, 0, 0, 0.7, 1.5, 2.4, 2.4, 2.4]
  ];
  for (let i = 0; i < size; i++) {
    points[i] = [];
    weights[i] = [];
    for (let j = 0; j < size; j++) {
      points[i][j] = [
        0.55 * i + 0.06 * i * i - 0.03 * i * j,
        0.48 * j + 0.05 * j * j + 0.02 * i * j,
        0.32 * Math.sin(0.7 * i + 0.4 * j) + 0.08 * i * j * 0.1
      ];
      weights[i][j] = 1 + 0.18 * Math.sin(0.9 * i + 0.6 * j) + 0.03 * i * j;
    }
  }
  return { points, weights, degree: [2, 2], knots, domain: [[0, 2.6], [0, 2.4]] };
}

// Planar surface with a deliberately non-affine parameterization.
function planarSpec () {
  const points = [];
  const weights = [];
  const size = 5;
  const knots = [
    [0, 0, 0, 0.8, 1.6, 2.4, 2.4, 2.4],
    [0, 0, 0, 0.6, 1.4, 2.2, 2.2, 2.2]
  ];
  for (let i = 0; i < size; i++) {
    points[i] = [];
    weights[i] = [];
    for (let j = 0; j < size; j++) {
      points[i][j] = [
        0.65 * i + 0.11 * i * i + 0.03 * i * j,
        0.5 * j + 0.09 * j * j - 0.02 * i * j,
        0
      ];
      weights[i][j] = 1 + 0.08 * Math.sin(i + j) + 0.01 * i * j;
    }
  }
  return { points, weights, degree: [2, 2], knots, domain: [[0, 2.4], [0, 2.2]] };
}

// Small single span quadratic patch (3x3) for dense KKT checks.
function smallSpec () {
  const points = [
    [[0.0, 0.0, 0.0], [0.0, 0.55, 0.08], [0.02, 1.1, 0.02]],
    [[0.55, 0.0, 0.1], [0.58, 0.58, 0.32], [0.6, 1.12, 0.12]],
    [[1.1, 0.02, 0.0], [1.12, 0.6, 0.18], [1.08, 1.15, 0.05]]
  ];
  const weights = [[1, 1.2, 1.1], [0.9, 1.4, 1.05], [1.1, 1.15, 1.3]];
  const knots = [[0, 0, 0, 1, 1, 1], [0, 0, 0, 1, 1, 1]];
  return { points, weights, degree: [2, 2], knots, domain: [[0, 1], [0, 1]] };
}

// A chosen displacement field, used to generate consistent targets.
function sampleDisplacement (i, j) {
  return [0.04 * Math.sin(1.3 * i + 0.7 * j), 0.05 * Math.cos(0.9 * i - 0.4 * j), 0.06 * Math.sin(0.5 * i + 1.1 * j)];
}

function targetFromDisplacement (surface, D, u, v) {
  const ev = evaluateRational(surface, u, v);
  const d = [0, 0, 0];
  for (const b of ev.basis) {
    const dd = D[b.i][b.j];
    for (let c = 0; c < 3; c++) d[c] += b.value * dd[c];
  }
  return [ev.S[0] + d[0], ev.S[1] + d[1], ev.S[2] + d[2]];
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

const cases = [];

// ===== metric ==============================================================

function metricCase (name, spec, anchors, shifts) {
  cases.push({
    group: 'metric',
    name,
    run (check, api) {
      const reference = makeSurface(api, spec);
      const plan = api.createFairingPlan(reference, { anchors, lambda: 1e-3 });
      const targets = anchors.map((a, k) => {
        const p = surfacePoint(reference, a[0], a[1]);
        return [p[0] + shifts[k][0], p[1] + shifts[k][1], p[2] + shifts[k][2]];
      });
      const result = plan.solve(targets);
      check(result.status === 'ok', 'solve status ok');
      check(result.surface !== null, 'surface returned');
      check(result.constraintResidual <= 1e-8, 'reported constraint residual below 1e-8');
      check(result.stationarityResidual <= 1e-8, 'reported stationarity residual below 1e-8');
      checkMetadata(check, result, reference);

      const D = displacementGrid(result.surface, reference, spec.degree && reference.size);
      const full = energyFromDisplacement(reference, 1e-3, D);
      const noGamma = energyFromDisplacement(reference, 1e-3, D, { gamma: false });
      const noMixed = energyFromDisplacement(reference, 1e-3, D, { mixed: false });

      const rel = Math.abs(full.energy - result.energy) / Math.max(1, Math.abs(result.energy));
      check(rel < 1e-9, 'reported energy matches independent quadrature: rel=' + rel);
      check(full.mass > 0, 'independent mass term is positive');
      check(Math.abs(full.energy - (full.bend + full.weightedMass)) < 1e-10 * Math.max(1, full.energy), 'energy split into bending and mass');

      const dGamma = Math.abs(noGamma.energy - full.energy) / Math.max(1e-12, full.bend);
      check(dGamma > 1e-3, 'Christoffel terms change the bending energy: rel=' + dGamma);
      const dMixed = Math.abs(noMixed.energy - full.energy) / Math.max(1e-12, full.bend);
      check(dMixed > 1e-3, 'the two mixed terms change the bending energy: rel=' + dMixed);

      // The same solution must reproduce the targets and its own geometry.
      for (let k = 0; k < anchors.length; k++) {
        const got = surfacePoint(result.surface, anchors[k][0], anchors[k][1]);
        for (let c = 0; c < 3; c++) {
          check(Math.abs(got[c] - targets[k][c]) < 1e-8, 'anchor ' + k + ' coordinate ' + c + ' meets target');
        }
      }
      return undefined;
    }
  });
}

metricCase('planar non-affine parameterisation',
  planarSpec(), [[0.7, 0.8], [1.9, 1.5]], [[0.05, -0.04, 0.18], [-0.03, 0.06, -0.12]]);
metricCase('curved rational surface',
  curvedSpec(), [[0.6, 0.5], [1.9, 1.7], [1.1, 2.0]], [[0.04, 0.07, 0.1], [-0.05, 0.02, 0.14], [0.02, -0.06, -0.09]]);

// Non-uniform knots on a curved surface, with degree 3 in one direction.
(function () {
  const points = [];
  const weights = [];
  const size = 5;
  for (let i = 0; i < size; i++) {
    points[i] = [];
    weights[i] = [];
    for (let j = 0; j < size; j++) {
      points[i][j] = [
        0.4 * i + 0.03 * i * i,
        0.45 * j + 0.04 * j * j,
        0.25 * Math.sin(0.6 * i) * Math.cos(0.5 * j) + 0.05 * i * j * 0.08
      ];
      weights[i][j] = 0.9 + 0.15 * Math.cos(0.4 * i + 0.7 * j) + 0.02 * i;
    }
  }
  metricCase('non-uniform knots with cubic degree',
    {
      points,
      weights,
      degree: [3, 2],
      knots: [[0, 0, 0, 0, 1.1, 2.2, 2.2, 2.2, 2.2], [0, 0, 0, 0.6, 1.3, 2.0, 2.0, 2.0]],
      domain: [[0, 2.2], [0, 2.0]]
    },
    [[0.6, 0.5], [1.7, 1.4]], [[0.05, -0.03, 0.12], [-0.04, 0.05, -0.1]]);
})();

// ===== constraints ========================================================

cases.push({
  group: 'constraints',
  name: 'interior drag matches a dense independent minimum',
  run (check, api) {
    const spec = smallSpec();
    const reference = makeSurface(api, spec);
    const lambda = 1e-2;
    const anchor = [0.47, 0.58];
    const p = surfacePoint(reference, anchor[0], anchor[1]);
    const target = [p[0] + 0.13, p[1] - 0.07, p[2] + 0.11];
    const plan = api.createFairingPlan(reference, { anchors: [anchor], lambda });
    const result = plan.solve([target]);
    check(result.status === 'ok', 'status ok');
    check(result.constraintResidual <= 1e-8, 'constraint residual small');
    check(result.stationarityResidual <= 1e-8, 'stationarity residual small');

    const ind = independentSolution(reference, lambda, [anchor], [target]);
    check(ind !== null, 'independent dense KKT solves');
    const D = displacementGrid(result.surface, reference, reference.size);
    let worst = 0;
    let k = 0;
    let energyPlan = 0;
    for (let i = 0; i < reference.size[0]; i++) {
      for (let j = 0; j < reference.size[1]; j++) {
        for (let c = 0; c < 3; c++) {
          worst = Math.max(worst, Math.abs(D[i][j][c] - ind.d[k++]));
        }
      }
    }
    check(worst < 1e-8, 'control displacement matches independent minimum: ' + worst);
    const rel = Math.abs(result.energy - ind.energy) / Math.max(1e-12, ind.energy);
    check(rel < 1e-7, 'energy matches independent minimum: rel=' + rel);
    void energyPlan;
    const got = surfacePoint(result.surface, anchor[0], anchor[1]);
    check(maxAbsDiff(got, target) < 1e-9, 'target exactly met');
    return undefined;
  }
});

cases.push({
  group: 'constraints',
  name: 'duplicate anchors and dependent rows at different UVs',
  run (check, api) {
    const spec = {
      points: [
        [[0, 0, 0], [0.02, 0.5, 0.1], [0, 1.0, 0], [0.02, 1.5, -0.08]],
        [[0.5, 0, 0.12], [0.53, 0.52, 0.25], [0.51, 1.02, 0.12], [0.5, 1.5, 0.05]],
        [[1.0, 0.01, 0], [1.02, 0.5, -0.05], [0.98, 1.0, 0.15], [1.0, 1.5, 0.02]],
        [[1.5, 0, 0.08], [1.52, 0.5, 0.2], [1.5, 1.0, 0.05], [1.48, 1.5, -0.1]]
      ],
      weights: [
        [1, 1.1, 1.2, 1.05], [1.15, 0.95, 1.3, 1.1],
        [1.05, 1.25, 1.0, 1.2], [1.2, 1.05, 1.1, 0.95]
      ],
      degree: [2, 2],
      knots: [[0, 0, 0, 1, 2, 2, 2], [0, 0, 0, 1, 2, 2, 2]],
      domain: [[0, 2], [0, 2]]
    };
    const reference = makeSurface(api, spec);
    const lambda = 5e-3;
    // All anchors fall inside the first knot rectangle (0,1)x(0,1), so the
    // active basis window has only 3x3 functions per axis: ten constraint
    // rows are necessarily linearly dependent.
    const anchors = [];
    for (const u of [0.12, 0.3, 0.48, 0.66, 0.84]) {
      for (const v of [0.22, 0.71]) anchors.push([u, v]);
    }
    // Add a duplicate at a different place in the list.
    anchors.push([0.3, 0.22]);
    const Dstar = [];
    for (let i = 0; i < reference.size[0]; i++) {
      Dstar[i] = [];
      for (let j = 0; j < reference.size[1]; j++) Dstar[i][j] = sampleDisplacement(i, j);
    }
    const targets = anchors.map((a) => targetFromDisplacement(reference, Dstar, a[0], a[1]));

    const plan = api.createFairingPlan(reference, { anchors, lambda });
    const result = plan.solve(targets);
    check(result.status === 'ok', 'dependent system solved');
    check(result.constraintResidual <= 1e-8, 'all dependent constraints satisfied');
    check(result.stationarityResidual <= 1e-8, 'stationarity residual small');
    for (let k = 0; k < anchors.length; k++) {
      const got = surfacePoint(result.surface, anchors[k][0], anchors[k][1]);
      check(maxAbsDiff(got, targets[k]) < 1e-8, 'dependent anchor ' + k + ' met');
    }

    const ind = independentSolution(reference, lambda, anchors, targets);
    check(ind !== null, 'independent KKT handles the dependent rows');
    check(ind.dependentRows > 0, 'constraint rows really were dependent');
    const D = displacementGrid(result.surface, reference, reference.size);
    let worst = 0; let k = 0;
    for (let i = 0; i < reference.size[0]; i++) {
      for (let j = 0; j < reference.size[1]; j++) {
        for (let c = 0; c < 3; c++) worst = Math.max(worst, Math.abs(D[i][j][c] - ind.d[k++]));
      }
    }
    check(worst < 1e-7, 'displacement matches independent minimum with dependent rows: ' + worst);

    // The duplicate is truly redundant: solving without it gives the same
    // geometry when its target agrees.
    const dedupAnchors = anchors.filter((a, i) => !(i === anchors.length - 1));
    const dedupTargets = targets.filter((t, i) => i !== anchors.length - 1);
    const plan2 = api.createFairingPlan(reference, { anchors: dedupAnchors, lambda });
    const result2 = plan2.solve(dedupTargets);
    check(result2.status === 'ok', 'deduplicated system solved');
    check(maxAbsDiff(result.surface.points, result2.surface.points) < 1e-8, 'duplicate row does not change the solution');
    return undefined;
  }
});

cases.push({
  group: 'constraints',
  name: 'conflicting and invalid inputs',
  run (check, api) {
    const spec = smallSpec();
    const reference = makeSurface(api, spec);
    const snapshot = copy(reference.points);
    const lambda = 1e-2;
    const a = [0.4, 0.5];
    const p = surfacePoint(reference, a[0], a[1]);
    const plan = api.createFairingPlan(reference, { anchors: [a, a], lambda });
    const r = plan.solve([[p[0] + 0.1, p[1], p[2]], [p[0] - 0.1, p[1], p[2]]]);
    check(r.status === 'inconsistent', 'conflicting duplicate targets reported inconsistent');
    check(r.surface === null, 'no surface on inconsistent solve');

    // Different UVs with targets not consistent with any displacement field.
    const anchors = [[0.12, 0.25], [0.31, 0.25], [0.52, 0.25], [0.73, 0.25]];
    const plan2 = api.createFairingPlan(reference, { anchors, lambda });
    const bogus = anchors.map((q, k) => {
      const pos = surfacePoint(reference, q[0], q[1]);
      return [pos[0] + 0.01 * ((k * 7) % 5), pos[1] + 0.02 * (k % 3), pos[2] - 0.03 * ((k * 3) % 4)];
    });
    const r2 = plan2.solve(bogus);
    check(r2.status === 'inconsistent', 'overdetermined inconsistent targets reported');
    check(r2.surface === null, 'surface null for overlapping inconsistent targets');

    const raises = (fn) => {
      try { fn(); return false; } catch (e) { return e instanceof RangeError; }
    };

    check(raises(() => api.createFairingPlan(reference, { anchors: [a], lambda: 1e-7 })), 'lambda below range rejected');
    check(raises(() => api.createFairingPlan(reference, { anchors: [a], lambda: 2 })), 'lambda above range rejected');
    check(raises(() => api.createFairingPlan(reference, { anchors: [a], lambda: NaN })), 'non-finite lambda rejected');
    check(raises(() => api.createFairingPlan(reference, { anchors: [], lambda })), 'empty anchors rejected');
    check(raises(() => api.createFairingPlan(reference, { anchors: [[1.4, 0.5]], lambda })), 'out of domain anchor rejected');
    check(raises(() => api.createFairingPlan(reference, { anchors: [[0.5, NaN]], lambda })), 'non-finite anchor rejected');
    check(raises(() => api.createFairingPlan(reference, { anchors: [[0.5]], lambda })), 'malformed anchor rejected');

    const okPlan = api.createFairingPlan(reference, { anchors: [a], lambda });
    check(raises(() => okPlan.solve([[0, 0]])), 'malformed target rejected');
    check(raises(() => okPlan.solve([[0, 0, 0], [1, 1, 1]])), 'wrong target count rejected');
    check(raises(() => okPlan.solve([[0, Infinity, 0]])), 'non-finite target rejected');

    // A rejected construction must not touch the source data.
    check(maxAbsDiff(reference.points, snapshot) === 0, 'failed construction left the source untouched');

    // Unsupported but structurally valid spline variants are rejected with
    // RangeError as well.
    const open = api.nurbs({ points: copy(spec.points), weights: copy(spec.weights), degree: [2, 2], knots: [spec.knots[0].slice(), spec.knots[1].slice()], boundary: ['open', 'open'] });
    check(raises(() => api.createFairingPlan(open, { anchors: [a], lambda })), 'non-clamped boundary rejected');
    const quadKnots = [[0, 0, 0.5, 1, 1], [0, 0, 0, 1, 1, 1]];
    const quad = api.nurbs({ points: copy(spec.points), weights: copy(spec.weights), degree: [1, 2], knots: quadKnots, boundary: ['clamped', 'clamped'] });
    check(raises(() => api.createFairingPlan(quad, { anchors: [a], lambda })), 'unsupported degree rejected');
    const badWeight = copy(spec.weights); badWeight[1][1] = -1;
    const neg = api.nurbs({ points: copy(spec.points), weights: badWeight, degree: [2, 2], knots: [spec.knots[0].slice(), spec.knots[1].slice()], boundary: ['clamped', 'clamped'] });
    check(raises(() => api.createFairingPlan(neg, { anchors: [a], lambda })), 'non-positive weight rejected');

    // Degenerate reference surface: all tangents parallel, so the integral
    // cannot be formed.
    const flatPoints = [
      [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
      [[0, 0, 0], [0, 0, 0], [0, 0, 0]],
      [[0, 0, 0], [0, 0, 0], [0, 0, 0]]
    ];
    const flat = api.nurbs({ points: flatPoints, weights: copy(spec.weights), degree: [2, 2], knots: [spec.knots[0].slice(), spec.knots[1].slice()], boundary: ['clamped', 'clamped'] });
    check(raises(() => api.createFairingPlan(flat, { anchors: [a], lambda })), 'degenerate integration points rejected');

    // Knot spacing and multiplicity restrictions.
    const tightKnots = [[0, 0, 0, 0.0005, 1, 1], [0, 0, 0, 1, 1, 1]];
    const tight = api.nurbs({ points: copy(spec.points), weights: copy(spec.weights), degree: [2, 2], knots: tightKnots, boundary: ['clamped', 'clamped'] });
    check(raises(() => api.createFairingPlan(tight, { anchors: [[0.0004, 0.4]], lambda })), 'knot spacing below 1e-3 rejected');
    return undefined;
  }
});

// ===== reuse ==============================================================

function refPositions (surface, anchors) {
  return anchors.map((a) => surfacePoint(surface, a[0], a[1]));
}

cases.push({
  group: 'reuse',
  name: 'A/B/A, source mutation, and result independence',
  run (check, api) {
    const spec = curvedSpec();
    const reference = makeSurface(api, spec);
    const anchors = [[0.55, 0.6], [1.75, 1.5]];
    const base = refPositions(reference, anchors);
    const A = [base.map((p) => [p[0] + 0.08, p[1] - 0.02, p[2] + 0.1])];
    const B = [base.map((p) => [p[0] - 0.05, p[1] + 0.06, p[2] - 0.12])];
    const lambda = 1e-3;
    const plan = api.createFairingPlan(reference, { anchors, lambda });

    const first = plan.solve(A[0]);
    const second = plan.solve(B[0]);
    const again = plan.solve(A[0]);
    check(first.status === 'ok' && second.status === 'ok' && again.status === 'ok', 'all solves ok');
    check(maxAbsDiff(first.surface.points, again.surface.points) < 1e-14, 'A/B/A reproduces A exactly');
    check(Math.abs(first.energy - again.energy) < 1e-14, 'A/B/A energy reproducible');
    check(first.surface !== again.surface, 'each solve returns a fresh surface');
    check(plan.assemblyCount === 1 && plan.factorizationCount === 1, 'assembly and factorization counts stay at one');

    // Solve A on a brand new plan created from the same reference: same result.
    const fresh = api.createFairingPlan(reference, { anchors, lambda });
    const freshA = fresh.solve(A[0]);
    check(maxAbsDiff(first.surface.points, freshA.surface.points) < 1e-12, 'result equals a fresh plan result');
    check(Math.abs(first.energy - freshA.energy) < 1e-12, 'fresh plan energy matches');

    // Results are independent of each other.
    const before = copy(second.surface.points);
    first.surface.transform([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.3, -0.2, 0.5, 1]);
    check(maxAbsDiff(second.surface.points, before) === 0, 'transforming one result leaves the other untouched');
    const afterPlan = plan.solve(A[0]);
    check(maxAbsDiff(afterPlan.surface.points, first.surface.points) > 0.1, 'the plan is not affected by transforming an old result');

    // Mutating the source after construction cannot reach the plan.
    const referenceSnapshot = copy(reference.points);
    for (let i = 0; i < reference.points.length; i++) {
      for (let j = 0; j < reference.points[i].length; j++) {
        reference.points[i][j][0] += 3.0;
        reference.points[i][j][1] -= 2.0;
        reference.points[i][j][2] += 1.0;
      }
    }
    reference.weights[0][0] = 5;
    reference.knots[0][0] = -4;
    const mutated = plan.solve(A[0]);
    check(mutated.status === 'ok', 'plan still solves after source mutation');
    check(maxAbsDiff(mutated.surface.points, again.surface.points) < 1e-14, 'source mutation does not change the plan result');
    check(mutated.surface.weights[0][0] !== 5, 'result uses the construction time weights');
    // Restore for the remaining checks (the plan does not need it).
    reference.points = referenceSnapshot;
    return undefined;
  }
});

cases.push({
  group: 'reuse',
  name: 'old evaluate/evaluator/transform API on the result',
  run (check, api) {
    const spec = curvedSpec();
    const reference = makeSurface(api, spec);
    const anchors = [[0.9, 0.8], [1.6, 1.7]];
    const base = refPositions(reference, anchors);
    const targets = base.map((p) => [p[0] - 0.04, p[1] + 0.09, p[2] + 0.13]);
    const plan = api.createFairingPlan(reference, { anchors, lambda: 2e-3 });
    const result = plan.solve(targets);
    const s = result.surface;

    for (let k = 0; k < anchors.length; k++) {
      const got = s.evaluate([], anchors[k][0], anchors[k][1]);
      check(maxAbsDiff(got, targets[k]) < 1e-8, 'old evaluate still meets the target');
      const independent = evaluateRational(s, anchors[k][0], anchors[k][1]);
      check(maxAbsDiff(got, independent.S) < 1e-10, 'old evaluate agrees with an independent rational evaluation');
    }

    const du = s.evaluator([1, 0]);
    const dv = s.evaluator([0, 1]);
    check(typeof du === 'function' && typeof dv === 'function', 'old first order evaluator works on the result');
    const u = 1.13; const v = 1.02;
    const h = 1e-5;
    const pu = s.evaluate([], u + h, v);
    const pm = s.evaluate([], u - h, v);
    check(maxAbsDiff(du([], u, v), pu.map((x, i) => (x - pm[i]) / (2 * h))) < 1e-6, 'first partial agrees with finite differences');
    check(maxAbsDiff(dv([], u, v), evaluateRational(s, u, v).Sv) < 1e-8, 'first partial agrees with independent quotient rule');

    const support = s.support([], u, v);
    check(support.length > 0, 'support is available');
    const before = copy(s.points);
    s.transform([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1, 0.2, -0.3, 1]);
    let moved = 0;
    for (let i = 0; i < s.points.length; i++) {
      for (let j = 0; j < s.points[i].length; j++) moved = Math.max(moved, Math.abs(s.points[i][j][2] - before[i][j][2] - (-0.3)));
    }
    check(moved < 1e-12, 'old transform still translates the result surface');
    return undefined;
  }
});

cases.push({
  group: 'reuse',
  name: 'rational second partials agree with the independent quotient rule',
  run (check, api) {
    // Both axes weighted, non-uniform knots, mixed second order included.
    const spec = curvedSpec();
    const surface = makeSurface(api, spec);
    const points = [
      [0.91, 0.73],
      [1.42, 1.61],
      [2.11, 0.31]
    ];
    const h = 1e-4;
    const partials = {
      uu: [2, 0],
      uv: [1, 1],
      vv: [0, 2],
      u: [1, 0],
      v: [0, 1]
    };
    const keys = Object.keys(partials);
    for (const [u, v] of points) {
      const independent = evaluateRational(surface, u, v);
      for (const key of keys) {
        const order = partials[key];
        const actual = surface.evaluator(order)([], u, v);
        const expected = key === 'u' ? independent.Su : key === 'v' ? independent.Sv : independent['S' + key];
        check(maxAbsDiff(actual, expected) < 1e-8,
          'executor ' + order.join(',') + ' matches the quotient rule at (' + u + ',' + v + ')');
      }
      // Second order by central differences as well, so the independent
      // quotient rule is itself cross checked.
      const fpp = surface.evaluate([], u + h, v + h);
      const fpm = surface.evaluate([], u + h, v - h);
      const fmp = surface.evaluate([], u - h, v + h);
      const fmm = surface.evaluate([], u - h, v - h);
      const fpd = surface.evaluate([], u + h, v);
      const fmd = surface.evaluate([], u - h, v);
      const fpe = surface.evaluate([], u, v + h);
      const fme = surface.evaluate([], u, v - h);
      const suu = [0, 0, 0];
      const suv = [0, 0, 0];
      const svv = [0, 0, 0];
      for (let c = 0; c < 3; c++) {
        suu[c] = (fpd[c] - 2 * surface.evaluate([], u, v)[c] + fmd[c]) / (h * h);
        svv[c] = (fpe[c] - 2 * surface.evaluate([], u, v)[c] + fme[c]) / (h * h);
        suv[c] = (fpp[c] - fpm[c] - fmp[c] + fmm[c]) / (4 * h * h);
      }
      check(maxAbsDiff(surface.evaluator([2, 0])([], u, v), suu) < 1e-5, 'second u partial matches finite differences');
      check(maxAbsDiff(surface.evaluator([0, 2])([], u, v), svv) < 1e-5, 'second v partial matches finite differences');
      check(maxAbsDiff(surface.evaluator([1, 1])([], u, v), suv) < 1e-5, 'mixed partial matches finite differences');
    }

    // The rational basis derivative must also be consistent: summing the
    // active basis derivatives recovers the geometry derivative.
    const [u0, v0] = points[0];
    const basis = surface.evaluator([0, 0], true);
    const basisU = surface.evaluator([1, 0], true);
    const basisV = surface.evaluator([0, 1], true);
    const basisUV = surface.evaluator([1, 1], true);
    const ev = evaluateRational(surface, u0, v0);
    const sum = [0, 0, 0];
    const sumU = [0, 0, 0];
    const sumV = [0, 0, 0];
    const sumUV = [0, 0, 0];
    for (const b of ev.basis) {
      const val = basis(u0, v0, b.i, b.j);
      const du = basisU(u0, v0, b.i, b.j);
      const dv = basisV(u0, v0, b.i, b.j);
      const duv = basisUV(u0, v0, b.i, b.j);
      check(Math.abs(val - b.value) < 1e-10, 'rational basis value matches independent evaluation');
      for (let c = 0; c < 3; c++) {
        sum[c] += val * surface.points[b.i][b.j][c];
        sumU[c] += du * surface.points[b.i][b.j][c];
        sumV[c] += dv * surface.points[b.i][b.j][c];
        sumUV[c] += duv * surface.points[b.i][b.j][c];
      }
    }
    check(maxAbsDiff(sum, ev.S) < 1e-10, 'basis values reproduce the point');
    check(maxAbsDiff(sumU, ev.Su) < 1e-9, 'basis u derivatives reproduce the first partial');
    check(maxAbsDiff(sumV, ev.Sv) < 1e-9, 'basis v derivatives reproduce the first partial');
    check(maxAbsDiff(sumUV, ev.Suv) < 1e-8, 'basis mixed derivatives reproduce the mixed partial');

    // Non-rational surfaces keep their existing generated second derivatives.
    const polySpec = {
      points: copy(spec.points),
      degree: [2, 2],
      knots: [spec.knots[0].slice(), spec.knots[1].slice()]
    };
    const poly = api.nurbs(polySpec);
    check(maxAbsDiff(poly.evaluator([2, 0])([], 0.8, 0.9), evaluateRational(poly, 0.8, 0.9).Suu) < 1e-8,
      'non-rational second derivatives are unchanged');
    check(maxAbsDiff(poly.evaluator([1, 1])([], 0.8, 0.9), evaluateRational(poly, 0.8, 0.9).Suv) < 1e-8,
      'non-rational mixed derivative is unchanged');
    return undefined;
  }
});

cases.push({
  group: 'reuse',
  name: 'interleaved plans',
  run (check, api) {
    const spec = smallSpec();
    const refA = makeSurface(api, spec);
    const refB = makeSurface(api, curvedSpec());
    const anchorsA = [[0.35, 0.45]];
    const anchorsB = [[0.8, 0.9]];
    const pA = surfacePoint(refA, anchorsA[0][0], anchorsA[0][1]);
    const pB = surfacePoint(refB, anchorsB[0][0], anchorsB[0][1]);
    const tA = [[pA[0] + 0.12, pA[1] - 0.05, pA[2] + 0.09]];
    const tB = [[pB[0] - 0.07, pB[1] + 0.11, pB[2] - 0.13]];
    const planA = api.createFairingPlan(refA, { anchors: anchorsA, lambda: 1e-2 });
    const planB = api.createFairingPlan(refB, { anchors: anchorsB, lambda: 1e-3 });
    const soloA = api.createFairingPlan(refA, { anchors: anchorsA, lambda: 1e-2 }).solve(tA);
    const soloB = api.createFairingPlan(refB, { anchors: anchorsB, lambda: 1e-3 }).solve(tB);

    const a1 = planA.solve(tA);
    const b1 = planB.solve(tB);
    const a2 = planA.solve(tA);
    const b2 = planB.solve(tB);
    check(maxAbsDiff(a1.surface.points, soloA.surface.points) < 1e-12, 'interleaved plan A matches its solo result');
    check(maxAbsDiff(b1.surface.points, soloB.surface.points) < 1e-12, 'interleaved plan B matches its solo result');
    check(maxAbsDiff(a2.surface.points, a1.surface.points) < 1e-14, 'plan A repeated after plan B is stable');
    check(maxAbsDiff(b2.surface.points, b1.surface.points) < 1e-14, 'plan B repeated after plan A is stable');
    check(planA.assemblyCount === 1 && planB.assemblyCount === 1, 'counts independent per plan');
    return undefined;
  }
});

function transformPoints (points, R, t) {
  return points.map((row) => row.map((p) => [
    R[0][0] * p[0] + R[0][1] * p[1] + R[0][2] * p[2] + t[0],
    R[1][0] * p[0] + R[1][1] * p[1] + R[1][2] * p[2] + t[1],
    R[2][0] * p[0] + R[2][1] * p[1] + R[2][2] * p[2] + t[2]
  ]));
}

cases.push({
  group: 'reuse',
  name: 'rigid motion, reparameterisation, and uniform weight scaling',
  run (check, api) {
    const spec = curvedSpec();
    const reference = makeSurface(api, spec);
    const anchors = [[0.7, 0.6], [1.8, 1.6]];
    const base = refPositions(reference, anchors);
    const targets = base.map((p) => [p[0] + 0.06, p[1] - 0.04, p[2] + 0.12]);
    const lambda = 1e-3;
    const plan = api.createFairingPlan(reference, { anchors, lambda });
    const result = plan.solve(targets);
    check(result.status === 'ok', 'reference solve ok');

    // Rigid motion: rotate and translate both the surface and the targets.
    const th = 0.37;
    const R = [
      [Math.cos(th), -Math.sin(th), 0],
      [Math.sin(th), Math.cos(th), 0],
      [0, 0, 1]
    ];
    const t = [0.4, -0.3, 0.7];
    const movedPoints = transformPoints(spec.points, R, t);
    const moved = api.nurbs({
      points: movedPoints,
      weights: copy(spec.weights),
      degree: spec.degree.slice(),
      knots: [spec.knots[0].slice(), spec.knots[1].slice()],
      boundary: ['clamped', 'clamped']
    });
    const movedTargets = targets.map((p) => [
      R[0][0] * p[0] + R[0][1] * p[1] + R[0][2] * p[2] + t[0],
      R[1][0] * p[0] + R[1][1] * p[1] + R[1][2] * p[2] + t[1],
      R[2][0] * p[0] + R[2][1] * p[1] + R[2][2] * p[2] + t[2]
    ]);
    const movedResult = api.createFairingPlan(moved, { anchors, lambda }).solve(movedTargets);
    check(movedResult.status === 'ok', 'rigidly moved solve ok');
    const expectMoved = transformPoints(result.surface.points, R, t);
    check(maxAbsDiff(movedResult.surface.points, expectMoved) < 1e-8, 'rigid motion maps the solution');

    // Positive affine reparameterisation of each axis independently.
    const au = 0.6; const bu = -0.9;
    const av = 1.4; const bv = 0.35;
    const reparamKnots = [
      spec.knots[0].map((x) => au * x + bu),
      spec.knots[1].map((x) => av * x + bv)
    ];
    const reparam = api.nurbs({
      points: copy(spec.points),
      weights: copy(spec.weights),
      degree: spec.degree.slice(),
      knots: reparamKnots,
      boundary: ['clamped', 'clamped']
    });
    const reparamAnchors = anchors.map((a) => [au * a[0] + bu, av * a[1] + bv]);
    const reparamResult = api.createFairingPlan(reparam, { anchors: reparamAnchors, lambda }).solve(targets);
    check(reparamResult.status === 'ok', 'reparameterised solve ok');
    check(maxAbsDiff(reparamResult.surface.points, result.surface.points) < 1e-8, 'affine reparameterisation leaves the solution unchanged');

    // Uniform positive weight scaling.
    const c = 3.7;
    const scaledWeights = spec.weights.map((row) => row.map((w) => w * c));
    const scaled = api.nurbs({
      points: copy(spec.points),
      weights: scaledWeights,
      degree: spec.degree.slice(),
      knots: [spec.knots[0].slice(), spec.knots[1].slice()],
      boundary: ['clamped', 'clamped']
    });
    const scaledResult = api.createFairingPlan(scaled, { anchors, lambda }).solve(targets);
    check(scaledResult.status === 'ok', 'weight scaled solve ok');
    check(maxAbsDiff(scaledResult.surface.points, result.surface.points) < 1e-8, 'uniform weight scaling leaves the geometry unchanged');
    check(maxAbsDiff(scaledResult.surface.weights, scaledWeights) < 1e-14, 'scaled weights are carried through');
    return undefined;
  }
});

module.exports = cases;
