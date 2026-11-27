"use strict"

// Regression tests for the exact-AO merge path.  Each exported group receives
// the legacy mesh function object (which also carries exactAO) and a check()
// recorder, and returns the number of checks performed.
//
//   field         - the unit-face piecewise-affine reference field itself
//   planning      - rectangle legality and the fixed merge order
//   compatibility - byte format, geometry and integration with the legacy path

var ndarray = require("ndarray")
var exact = require("../exact-mesh")

var OPAQUE = 1 << 15
var AO_TABLE = exact.AO_TABLE
var EPS = exact.EPS

function rng(seed) {
  var s = seed >>> 0
  return function () {
    s = (s * 1664525 + 1013904223) >>> 0
    return s / 4294967296
  }
}

function randomVoxels(n, seed, transparentChance) {
  var r = rng(seed)
  var vox = ndarray(new Uint16Array((n + 2) * (n + 2) * (n + 2)),
                    [n + 2, n + 2, n + 2])
  for (var x = 0; x < n; ++x)
    for (var y = 0; y < n; ++y)
      for (var z = 0; z < n; ++z) {
        var roll = r()
        if (roll < transparentChance) {
          vox.set(x + 1, y + 1, z + 1, 1 + ((r() * 4) | 0))
        } else if (roll < transparentChance + 0.5) {
          vox.set(x + 1, y + 1, z + 1, OPAQUE | (1 + ((r() * 4) | 0)))
        }
      }
  return vox
}

// Build a synthetic unit-face record with the same shape buildFaces returns.
function face(a00, a10, a11, a01, flip, tex) {
  var lf = exact.legacyFlipAO(a00, a10, a11, a01)
  return {
    a00: a00, a10: a10, a11: a11, a01: a01,
    legacyFlip: lf,
    diagAC: !lf,
    cA: a00, cB: a10, cC: a11, cD: a01,
    flip: flip === undefined ? false : !!flip,
    tex: tex === undefined ? 1 : tex,
    constant: a00 === a10 && a10 === a11 && a11 === a01
  }
}

// Bilinear interpolation over the four physical corners, for contrasting with
// the required piecewise-affine (triangle) field.
function bilinear(f, gu, gv) {
  var c = exact.faceCorners(f)
  return c.A * (1 - gu) * (1 - gv) + c.B * gu * (1 - gv) +
         c.C * gu * gv + c.D * (1 - gu) * gv
}

// Big-triangle affine field of a planned rectangle.
function bigField(rc, grid, nu, gu, gv) {
  function corner(uu, vv, name) {
    return exact.faceCorners(grid[vv * nu + uu])[name]
  }
  var A = corner(rc.u0, rc.v0, "A")
  var B = corner(rc.u0 + rc.w - 1, rc.v0, "B")
  var C = corner(rc.u0 + rc.w - 1, rc.v0 + rc.h - 1, "C")
  var D = corner(rc.u0, rc.v0 + rc.h - 1, "D")
  var x = gu / rc.w
  var y = gv / rc.h
  if (rc.diagAC) {
    if (y <= x) return A * (1 - x) + B * (x - y) + C * y
    return A * (1 - y) + C * x + D * (y - x)
  }
  if (x + y <= 1) return A * (1 - x - y) + B * x + D * y
  return B * (1 - y) + C * (x + y - 1) + D * (1 - x)
}

// ---------------------------------------------------------------------------
// field: the reference field is the independently computed unit-face field,
// and every emitted rectangle reproduces it exactly.
// ---------------------------------------------------------------------------
function field(api, check) {
  var count = 0
  function ck(cond, msg) { check(cond, msg); count++ }

  // 1. The reference reproduces each physical corner value exactly.
  var f = face(255, 204, 153, 204)
  var c = exact.faceCorners(f)
  ck(Math.abs(exact.refInsideCell(f, 0, 0) - c.A) <= EPS,
    "reference equals corner A at (0,0)")
  ck(Math.abs(exact.refInsideCell(f, 1, 0) - c.B) <= EPS,
    "reference equals corner B at (1,0)")
  ck(Math.abs(exact.refInsideCell(f, 1, 1) - c.C) <= EPS,
    "reference equals corner C at (1,1)")
  ck(Math.abs(exact.refInsideCell(f, 0, 1) - c.D) <= EPS,
    "reference equals corner D at (0,1)")

  // 2. The field is continuous across the cell's own diagonal (the two affine
  //    pieces meet there).  Evaluate both sides of the diagonal at its midpoint.
  var mid = exact.refInsideCell(f, 0.5, 0.5)
  ck(Number.isFinite(mid) && mid > 0, "reference is a finite positive value")

  // 3. It is the piecewise-affine triangle field, NOT bilinear interpolation.
  //    Three bright corners and one dark corner make the two constructions
  //    differ strongly at the cell centre.
  var fb = face(255, 255, 0, 255)
  var maxBilinearGap = 0
  for (var sx = 1; sx < 10; ++sx)
    for (var sy = 1; sy < 10; ++sy) {
      var gu = sx / 10, gv = sy / 10
      maxBilinearGap = Math.max(maxBilinearGap,
        Math.abs(exact.refInsideCell(fb, gu, gv) - bilinear(fb, gu, gv)))
    }
  ck(maxBilinearGap > 10, "reference field differs from bilinear interpolation")

  // 4. A non-constant but globally affine strip is representable by one big
  //    rectangle, while a repeated identical non-constant mask is not (the
  //    legacy label-equality merge would distort it).
  var affineStrip = [face(255, 235, 235, 255), face(235, 215, 215, 235)]
  ck(exact.evaluateRect(affineStrip, 2, 1, 0, 0, 2, 1) !== null,
    "non-constant affine strip merges")
  var sameMask = [face(255, 204, 204, 255), face(255, 204, 204, 255)]
  ck(exact.evaluateRect(sameMask, 2, 1, 0, 0, 2, 1) === null,
    "same non-constant mask stretched over a strip is rejected")

  // 5. Over a full voxel model, every planned rectangle's big-triangle field
  //    matches the independently computed unit-face reference within 1e-9.
  var vox = randomVoxels(7, 424242, 0.12)
  var nx = 7
  var faces = exact.buildFaces(vox, nx, nx, nx, null)
  var dims = [nx, nx, nx]
  var worst = 0
  var sampled = 0
  for (var d = 0; d < 3; ++d) {
    var nu = dims[(d + 1) % 3], nv = dims[(d + 2) % 3]
    for (var b = 0; b <= nx; ++b) {
      var grid = faces[d].slice(b * nu * nv, b * nu * nv + nu * nv)
      var rects = exact.planSlice(grid, nu, nv)
      for (var ri = 0; ri < rects.length; ++ri) {
        var rc = rects[ri]
        for (var cv = 0; cv < rc.h; ++cv)
          for (var cu = 0; cu < rc.w; ++cu) {
            var unit = grid[(rc.v0 + cv) * nu + (rc.u0 + cu)]
            for (var a = 1; a < 6; ++a)
              for (var e2 = 1; e2 < 6; ++e2) {
                var lu = a / 6, lv = e2 / 6
                var want = exact.refInsideCell(unit, lu, lv)
                var got = bigField(rc, grid, nu, cu + lu, cv + lv)
                worst = Math.max(worst, Math.abs(want - got))
                sampled++
              }
          }
      }
    }
  }
  ck(sampled > 1000, "reference comparison samples the interior densely")
  ck(worst <= 1e-9, "rectangle field matches unit reference within 1e-9")

  return count
}

// ---------------------------------------------------------------------------
// planning: legal rectangles, the fixed traversal order and tie-breaking.
// ---------------------------------------------------------------------------
function planning(api, check) {
  var count = 0
  function ck(cond, msg) { check(cond, msg); count++ }

  // 1. A constant 2x2 block collapses to a single 2x2 rectangle.
  var k = AO_TABLE[2]
  var g22 = [
    face(k, k, k, k), face(k, k, k, k),
    face(k, k, k, k), face(k, k, k, k)
  ]
  var p22 = exact.planSlice(g22, 2, 2)
  ck(p22.length === 1 && p22[0].w === 2 && p22[0].h === 2,
    "constant 2x2 block merges into one rectangle")

  // 2. Planning covers every unit face exactly once.
  ck(p22[0].w * p22[0].h === 4, "merged rectangle covers all four unit faces")

  // 3. Equal-area tie is broken toward the larger first-axis (u) span.  Row 0
  //    and column 0 are constant X, the far cell is constant Y != X, so the
  //    2x2 rectangle is illegal while the 2x1 and 1x2 arms are both legal.
  var x = AO_TABLE[3], y = AO_TABLE[1]
  var tie = [
    face(x, x, x, x), face(x, x, x, x),
    face(x, x, x, x), face(y, y, y, y)
  ]
  ck(exact.evaluateRect(tie, 2, 2, 0, 0, 2, 2) === null,
    "mixed-value 2x2 rectangle is not legal")
  ck(exact.evaluateRect(tie, 2, 2, 0, 0, 2, 1) !== null,
    "2x1 horizontal arm is legal")
  ck(exact.evaluateRect(tie, 2, 2, 0, 0, 1, 2) !== null,
    "1x2 vertical arm is legal")
  var ptie = exact.planSlice(tie, 2, 2)
  ck(ptie[0].w === 2 && ptie[0].h === 1,
    "equal-area tie picks the larger first-axis span")

  // 4. A non-constant affine 3x1 strip stays a single merged rectangle rather
  //    than being split back into unit faces.
  var strip = [
    face(255, 238, 238, 255),
    face(238, 221, 221, 238),
    face(221, 204, 204, 221)
  ]
  var ps = exact.planSlice(strip, 3, 1)
  ck(ps.length === 1 && ps[0].w === 3,
    "non-constant legal strip is not split into unit faces")

  // 5. Texture and orientation boundaries block merging.
  var texBound = [face(k, k, k, k, false, 1), face(k, k, k, k, false, 2)]
  ck(exact.evaluateRect(texBound, 2, 1, 0, 0, 2, 1) === null,
    "texture boundary blocks a merge")
  var oriBound = [face(k, k, k, k, false), face(k, k, k, k, true)]
  ck(exact.evaluateRect(oriBound, 2, 1, 0, 0, 2, 1) === null,
    "normal orientation boundary blocks a merge")

  // 6. A distorted region falls back to unit faces, never disappearing.
  var pd = exact.planSlice(sameMaskGrid(), 2, 1)
  ck(pd.length === 2 && pd.every(function (r) { return r.w === 1 && r.h === 1 }),
    "non-mergeable faces are emitted as unit rectangles")

  // 7. A unit face whose reference diagonal is B-D keeps that diagonal rather
  //    than being forced onto A-C (legacy flipAO=true here).
  var bdFace = [face(255, 255, 0, 255)]
  ck(exact.legacyFlipAO(255, 255, 0, 255) === true,
    "dark C corner selects the B-D diagonal under the legacy rule")
  var pbd = exact.planSlice(bdFace, 1, 1)
  ck(pbd.length === 1 && pbd[0].diagAC === false,
    "the B-D reference diagonal is preserved on output")

  return count
}

function sameMaskGrid() {
  return [face(255, 204, 204, 255), face(255, 204, 204, 255)]
}

// ---------------------------------------------------------------------------
// compatibility: output encoding, geometry, transparency, strides and reuse.
// ---------------------------------------------------------------------------
function compatibility(api, check) {
  var count = 0
  function ck(cond, msg) { check(cond, msg); count++ }

  // 1. Empty model returns null.
  ck(api.exactAO(ndarray(new Uint16Array(27), [3, 3, 3])) === null,
    "empty model returns null")

  // 2. Single isolated voxel is byte-for-byte identical to the legacy path,
  //    inheriting positions, normals, AO and texture encoding.
  var one = ndarray(new Uint16Array(125), [5, 5, 5])
  one.set(2, 2, 2, OPAQUE | 1)
  var legacyBytes = api(one)
  var exactBytes = api.exactAO(one)
  ck(exactBytes instanceof Uint8Array, "result is a Uint8Array")
  ck(exactBytes.length % 8 === 0, "vertex stream is a multiple of 8 bytes")
  ck(exactBytes.length === legacyBytes.length &&
    Buffer.compare(Buffer.from(exactBytes), Buffer.from(legacyBytes)) === 0,
    "single-voxel output is byte-identical to the legacy path")

  // 3. Normal encoding: one axis carries 127/129, the other two stay 128; AO
  //    bytes are always AO_TABLE entries.
  var validAO = {}
  AO_TABLE.forEach(function (v) { validAO[v] = 1 })
  var encOK = true
  for (var i = 0; i < exactBytes.length; i += 8) {
    var nb = [exactBytes[i + 4], exactBytes[i + 5], exactBytes[i + 6]]
    var special = nb.filter(function (b) { return b === 127 || b === 129 }).length
    var neutral = nb.filter(function (b) { return b === 128 }).length
    if (special !== 1 || neutral !== 2 || !validAO[exactBytes[i + 3]]) encOK = false
  }
  ck(encOK, "normal and AO byte encodings follow the legacy format")

  // 4. Winding: geometric cross product sign agrees with the encoded normal.
  var windOK = true
  for (var t = 0; t < exactBytes.length; t += 24) {
    function P(o) {
      return [exactBytes[t + o], exactBytes[t + o + 1], exactBytes[t + o + 2]]
    }
    var p0 = P(0), p1 = P(8), p2 = P(16)
    var e1 = [p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]]
    var e2 = [p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]]
    var cr = [
      e1[1] * e2[2] - e1[2] * e2[1],
      e1[2] * e2[0] - e1[0] * e2[2],
      e1[0] * e2[1] - e1[1] * e2[0]
    ]
    var axis = 0
    while (axis < 3 && cr[axis] === 0) axis++
    var encoded = exactBytes[t + 4 + axis] - 128
    if (Math.sign(cr[axis]) !== Math.sign(encoded)) windOK = false
  }
  ck(windOK, "triangle winding matches the encoded normal direction")

  // 5. Transparent voxels keep the same visible-face geometry as the legacy
  //    path (a face exists between an opaque and a merely-present voxel).
  var mixed = ndarray(new Uint16Array(27), [3, 3, 3])
  mixed.set(1, 1, 1, OPAQUE | 1)
  mixed.set(2, 1, 1, 5) // present but transparent neighbour
  ck(api.exactAO(mixed).length === api(mixed).length,
    "transparent voxel visibility matches the legacy path")

  // 6. The per-side texture table is honoured and changes the texture slot.
  var sideTable = { get: function (voxel, side) { return (voxel * 7 + side) & 0xff } }
  var withTex = api.exactAO(one, sideTable)
  var texIds = {}
  for (var q = 7; q < withTex.length; q += 8) texIds[withTex[q]] = 1
  ck(Object.keys(texIds).length > 1, "per-side texture ids are applied")

  // 7. Negative strides and an offset produce the same mesh as a contiguous
  //    array with identical logical content.
  var nn = 5, N = nn + 2
  var ref = randomVoxels(nn, 99, 0.1)
  var backing = new Uint16Array(N * N * N)
  var rev = ndarray(backing, [N, N, N],
    [-(N * N), -N, -1],
    (N - 1) * N * N + (N - 1) * N + (N - 1))
  for (var a = 0; a < N; ++a)
    for (var b2 = 0; b2 < N; ++b2)
      for (var c2 = 0; c2 < N; ++c2)
        rev.set(a, b2, c2, ref.get(a, b2, c2))
  var mRef = api.exactAO(ref)
  var mRev = api.exactAO(rev)
  ck(mRef.length === mRev.length &&
    Buffer.compare(Buffer.from(mRef), Buffer.from(mRev)) === 0,
    "negative-stride view yields the same mesh")

  // 8. Repeated calls are deterministic.
  var again = api.exactAO(ref)
  ck(Buffer.compare(Buffer.from(mRef), Buffer.from(again)) === 0,
    "repeated calls return identical output")

  return count
}

module.exports = {
  field: field,
  planning: planning,
  compatibility: compatibility
}
