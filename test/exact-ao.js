"use strict"

//Regression tests for the exact AO mesher.  Each exported group receives the
//legacy mesh function object (which also carries the shared internals and
//exactAO) and a cc(condition, message) recorder, and returns the number of
//check calls it made.  A failed check throws.

var ndarray = require("ndarray")
var compileMesher = require("greedy-mesher")
var exact = require("../exact-mesh.js")

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

function rng(seed) {
  return function() {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff
    return seed / 0x7fffffff
  }
}

//Pack physical-frame corner intensities into one surface-voxel value.
//Slot frame is transposed relative to the physical frame: slot0=c00,
//slot1=c01, slot2=c11, slot3=c10 (see buildGrid).
function packFace(api, c00, c10, c11, c01, low) {
  var T = api.AO_TABLE
  function ix(v) { return T.indexOf(v) }
  return (ix(c00) << api.AO_SHIFT) |
         (ix(c01) << (api.AO_SHIFT + api.AO_BITS)) |
         (ix(c11) << (api.AO_SHIFT + 2 * api.AO_BITS)) |
         (ix(c10) << (api.AO_SHIFT + 3 * api.AO_BITS)) |
         (low || 1)
}

//Physical-frame corners of a packed surface value.
function corners(api, val) {
  var T = api.AO_TABLE, SH = api.AO_SHIFT, B = api.AO_BITS, M = api.AO_MASK
  return {
    c00: T[(val >>> SH) & M],
    c10: T[(val >>> (SH + 3 * B)) & M],
    c11: T[(val >>> (SH + 2 * B)) & M],
    c01: T[(val >>> (SH + B)) & M]
  }
}

function faceFlipOf(c) {
  var f = c.c00 + c.c11 < c.c10 + c.c01
  if(c.c00 + c.c11 === c.c10 + c.c01) {
    f = Math.max(c.c00, c.c11) < Math.max(c.c10, c.c01)
  }
  return f
}

//Unit-face reference field: two triangles, barycentric (affine) interpolation.
//Computed independently from the packed value, not from the mesher.
function refAt(api, val, x, y) {
  var c = corners(api, val), f = faceFlipOf(c)
  if(f) {
    if(x + y <= 1) {
      return c.c00 + (c.c10 - c.c00) * x + (c.c01 - c.c00) * y
    }
    return (c.c10 + c.c01 - c.c11) +
           (c.c11 - c.c01) * x + (c.c11 - c.c10) * y
  }
  if(x >= y) {
    return c.c00 + (c.c10 - c.c00) * x + (c.c11 - c.c10) * y
  }
  return c.c00 + (c.c11 - c.c01) * x + (c.c01 - c.c00) * y
}

//Parse an 8-byte vertex stream into quads.
function parseQuads(bytes) {
  var quads = []
  for(var p = 0; p < bytes.length; p += 48) {
    var verts = []
    for(var k = 0; k < 6; ++k) {
      var o = p + k * 8
      verts.push({
        x: bytes[o], y: bytes[o + 1], z: bytes[o + 2], ao: bytes[o + 3],
        nx: bytes[o + 4], ny: bytes[o + 5], nz: bytes[o + 6], tex: bytes[o + 7]
      })
    }
    var d = verts[0].nx !== 128 ? 0 : (verts[0].ny !== 128 ? 1 : 2)
    var u = (d + 1) % 3, v = (d + 2) % 3
    var ax = ["x", "y", "z"]
    var us = verts.map(function(t) { return t[ax[u]] })
    var vs = verts.map(function(t) { return t[ax[v]] })
    quads.push({
      d: d, u: u, v: v, z: verts[0][ax[d]], verts: verts,
      u0: Math.min.apply(null, us), u1: Math.max.apply(null, us),
      v0: Math.min.apply(null, vs), v1: Math.max.apply(null, vs)
    })
  }
  return quads
}

//Field value of a rendered quad at a physical point, from its two triangles.
function quadAt(q, x, y) {
  var ax = ["x", "y", "z"]
  function tri(vs) {
    return vs.map(function(t) { return {x: t[ax[q.u]], y: t[ax[q.v]], a: t.ao} })
  }
  var t1 = tri(q.verts.slice(0, 3)), t2 = tri(q.verts.slice(3, 6))
  function bary(A, B, C) {
    var den = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y)
    var a = ((B.y - C.y) * (x - C.x) + (C.x - B.x) * (y - C.y)) / den
    var b = ((C.y - A.y) * (x - C.x) + (A.x - C.x) * (y - C.y)) / den
    return a * A.a + b * B.a + (1 - a - b) * C.a
  }
  function inside(A, B, C) {
    function s(P, Q) {
      return (x - Q.x) * (P.y - Q.y) - (P.x - Q.x) * (y - Q.y)
    }
    var d1 = s(A, B), d2 = s(B, C), d3 = s(C, A)
    return (d1 >= -1e-12 && d2 >= -1e-12 && d3 >= -1e-12) ||
           (d1 <= 1e-12 && d2 <= 1e-12 && d3 <= 1e-12)
  }
  return inside(t1[0], t1[1], t1[2])
    ? bary(t1[0], t1[1], t1[2])
    : bary(t2[0], t2[1], t2[2])
}

//Run the surface stencil and return per-direction transposed slice grids.
function faceGrids(api, array) {
  var shp = array.shape
  var dims = [shp[0] - 2, shp[1] - 2, shp[2] - 2]
  var N = dims[0] * dims[1] * dims[2]
  var ao = [
    ndarray(new Int32Array(N), dims),
    ndarray(new Int32Array(N), dims),
    ndarray(new Int32Array(N), dims)
  ]
  api.surfaceStencil(ao[0], ao[1], ao[2], array)
  return ao
}

function randomScene(api, seed, S, density, withTransparent) {
  var r = rng(seed)
  var a = ndarray(new Uint16Array((S + 2) * (S + 2) * (S + 2)), [S + 2, S + 2, S + 2])
  for(var i = 1; i <= S; ++i)
    for(var j = 1; j <= S; ++j)
      for(var k = 1; k <= S; ++k) {
        var roll = r()
        if(roll < density) {
          a.set(i, j, k, api.OPAQUE_BIT | (1 + Math.floor(r() * 4)))
        } else if(withTransparent && roll < density + 0.1) {
          //Truthy but non-opaque: a transparent voxel.
          a.set(i, j, k, 3 + Math.floor(r() * 3))
        }
      }
  return a
}

//Floor slab plus one raised wall row: yields a long non-constant but exactly
//mergeable AO strip on the floor's top face.
function stripScene(api, S) {
  var a = ndarray(new Uint16Array((S + 2) * (S + 2) * (S + 2)), [S + 2, S + 2, S + 2])
  for(var i = 1; i <= S; ++i)
    for(var j = 1; j <= S; ++j) a.set(i, j, 1, api.OPAQUE_BIT | 1)
  for(i = 1; i <= S; ++i) a.set(i, 1, 2, api.OPAQUE_BIT | 1)
  return a
}

// ---------------------------------------------------------------------------
// Group 1: the preserved reference field
// ---------------------------------------------------------------------------

function field(api, check) {
  var calls = 0
  var _check = check
  function cc(cond, msg) { _check(cond, msg); calls++ }

  //1. Single voxel: exact path is byte-identical to the legacy path.
  var one = ndarray(new Uint16Array(125), [5, 5, 5])
  one.set(2, 2, 2, api.OPAQUE_BIT | 1)
  var legacyBytes = api(one)
  var exactBytes = api.exactAO(one)
  cc(exactBytes instanceof Uint8Array &&
        Buffer.compare(Buffer.from(legacyBytes), Buffer.from(exactBytes)) === 0,
        "single voxel exact output equals legacy output")

  //2. Empty model returns null.
  var empty = ndarray(new Uint16Array(27), [3, 3, 3])
  cc(api.exactAO(empty) === null, "empty model returns null")

  //3. Reference is barycentric-affine, not bilinear.  Use a non-flip face
  //   (c00-c11 diagonal); at (0.5,0.5) on that diagonal the affine value is
  //   (c00+c11)/2, which differs from the bilinear four-corner average.
  var c00 = 255, c10 = 153, c11 = 255, c01 = 204
  var val = packFace(api, c00, c10, c11, c01)
  cc(faceFlipOf(corners(api, val)) === false, "probe face uses the c00-c11 diagonal")
  var affine = refAt(api, val, 0.5, 0.5)
  var bilinear = (c00 + c10 + c11 + c01) / 4
  cc(Math.abs(affine - (c00 + c11) / 2) < 1e-12,
        "reference uses affine triangle interpolation")
  cc(Math.abs(affine - bilinear) > 1e-9,
        "reference is not the bilinear corner average")

  //4. On random scenes every rendered quad reproduces the unit-face
  //   reference field at dense off-lattice samples (1e-9 tolerance).
  var samples = [[0.13, 0.37], [0.5, 0.5], [0.62, 0.88], [0.88, 0.13]]
  for(var t = 0; t < 3; ++t) {
    var S = 7
    var scene = randomScene(api, 100 + t, S, 0.4, true)
    var bytes = api.exactAO(scene)
    var ao = faceGrids(api, scene)
    var quads = parseQuads(bytes)
    var worst = 0
    quads.forEach(function(q) {
      var sl = ao[q.d].transpose(q.d, q.u, q.v).pick(q.z)
      for(var r = q.v0; r < q.v1; ++r) {
        for(var qq = q.u0; qq < q.u1; ++qq) {
          var fv = sl.get(qq, r)
          samples.forEach(function(pt) {
            var got = quadAt(q, qq + pt[0], r + pt[1])
            var want = refAt(api, fv, pt[0], pt[1])
            worst = Math.max(worst, Math.abs(got - want))
          })
        }
      }
    })
    cc(worst <= 1e-9, "random scene " + t + " field exact within 1e-9 (worst " + worst + ")")
  }

  //5. A non-constant strip is actually merged into a rectangle, and the
  //   merged field still matches the per-face reference.
  var strip = stripScene(api, 8)
  var sBytes = api.exactAO(strip)
  var sAo = faceGrids(api, strip)
  var sQuads = parseQuads(sBytes)
  var merged = sQuads.filter(function(q) {
    if((q.u1 - q.u0) <= 1 && (q.v1 - q.v0) <= 1) return false
    var seen = {}, aos = []
    q.verts.forEach(function(vt) {
      var key = vt.x + "," + vt.y + "," + vt.z
      if(!seen[key]) { seen[key] = 1; aos.push(vt.ao) }
    })
    return aos.some(function(x) { return x !== aos[0] })
  })
  cc(merged.length > 0, "a non-constant strip was merged into a rectangle")
  var stripWorst = 0
  sQuads.forEach(function(q) {
    var sl = sAo[q.d].transpose(q.d, q.u, q.v).pick(q.z)
    for(var r = q.v0; r < q.v1; ++r) {
      for(var qq = q.u0; qq < q.u1; ++qq) {
        var fv = sl.get(qq, r)
        samples.forEach(function(pt) {
          stripWorst = Math.max(stripWorst,
            Math.abs(quadAt(q, qq + pt[0], r + pt[1]) - refAt(api, fv, pt[0], pt[1])))
        })
      }
    }
  })
  cc(stripWorst <= 1e-9, "merged non-constant strip keeps the exact field")

  return calls
}

// ---------------------------------------------------------------------------
// Group 2: merge planning order and decisions
// ---------------------------------------------------------------------------

function planning(api, check) {
  var calls = 0
  var _check = check
  function cc(cond, msg) { _check(cond, msg); calls++ }

  //1. A flat constant grid is collapsed to one maximal rectangle.
  var flatVal = packFace(api, 255, 255, 255, 255)
  var flat = ndarray(new Int32Array(12), [4, 3])
  for(var i = 0; i < 12; ++i) flat.data[i] = flatVal
  var flatGrid = exact.buildGrid(flat, 4, 3, 2, null)
  var flatRects = exact.planSlice(flatGrid)
  cc(flatRects.length === 1 &&
        flatRects[0].q0 === 0 && flatRects[0].r0 === 0 &&
        flatRects[0].q1 === 4 && flatRects[0].r1 === 3,
        "flat region becomes one maximal 4x3 rectangle")

  //2. A U-gradient strip that cannot be expressed by one big rectangle is
  //   kept as unit faces rather than wrongly stretched (no wholesale
  //   fall-back to unit faces for constant regions, checked separately).
  var gradVal = packFace(api, 153, 204, 255, 153)
  var grad = ndarray(new Int32Array(4), [4, 1])
  for(i = 0; i < 4; ++i) grad.data[i] = gradVal
  var gradGrid = exact.buildGrid(grad, 4, 1, 2, null)
  var gradRects = exact.planSlice(gradGrid)
  cc(gradRects.length === 4 &&
        gradRects.every(function(rc) { return rc.q1 - rc.q0 === 1 && rc.r1 - rc.r0 === 1; }),
        "unmergeable non-constant strip stays as four unit faces")

  //3. Tie of equal area is broken toward the larger first-axis (q) span.
  //   Row r0 has cells q0,q1; row r1 has only q0 -> from seed (0,0) the
  //   2x1 and 1x2 rectangles both have area 2.
  var tie = ndarray(new Int32Array(4), [2, 2])
  tie.set(0, 0, flatVal); tie.set(1, 0, flatVal); tie.set(0, 1, flatVal)
  var tieGrid = exact.buildGrid(tie, 2, 2, 2, null)
  var tieRects = exact.planSlice(tieGrid)
  cc(tieRects[0].q1 - tieRects[0].q0 === 2 &&
        tieRects[0].r1 - tieRects[0].r0 === 1,
        "equal-area tie picks the wider (larger first-axis span) rectangle")

  //4. The seed is the first uncovered cell scanning r then q; rectangles
  //   partition every visible cell exactly once.
  var scene = randomScene(api, 333, 8, 0.42, false)
  var ao = faceGrids(api, scene)
  var totalVisible = 0, coveredOnce = 0
  for(var d = 0; d < 3; ++d) {
    var u = (d + 1) % 3, v = (d + 2) % 3, dims = [8, 8, 8]
    for(var si = 0; si < dims[d]; ++si) {
      var sl = ao[d].transpose(d, u, v).pick(si)
      var grid = exact.buildGrid(sl, dims[u], dims[v], d, null)
      var mark = new Int32Array(dims[u] * dims[v])
      exact.planSlice(grid).forEach(function(rc) {
        for(var r = rc.r0; r < rc.r1; ++r)
          for(var q = rc.q0; q < rc.q1; ++q) mark[r * dims[u] + q]++
      })
      for(var r2 = 0; r2 < dims[v]; ++r2)
        for(var q2 = 0; q2 < dims[u]; ++q2) {
          if(sl.get(q2, r2)) {
            totalVisible++
            if(mark[r2 * dims[u] + q2] === 1) coveredOnce++
          } else {
            cc(mark[r2 * dims[u] + q2] === 0, "hidden cell is never covered")
          }
        }
    }
  }
  cc(totalVisible > 0 && coveredOnce === totalVisible,
        "planned rectangles cover every visible cell exactly once")

  //5. The non-constant mergeable strip is planned as a wide rectangle with
  //   non-uniform corner intensities (proves not everything is split up).
  var stripAo = faceGrids(api, stripScene(api, 8))
  var stripSl = stripAo[2].transpose(2, 0, 1).pick(1)
  var stripGrid = exact.buildGrid(stripSl, 8, 8, 2, null)
  var stripRects = exact.planSlice(stripGrid)
  var wide = stripRects.filter(function(rc) { return rc.q1 - rc.q0 > 1; })
  cc(wide.length > 0 &&
        wide.some(function(rc) {
          var c = rc.corners
          return c.a00 !== c.a10 || c.a00 !== c.a01 || c.a00 !== c.a11
        }),
        "planning keeps a wide non-constant but exact rectangle")

  //6. Both diagonals are evaluated independently; a face only expressible
  //   with the c01-c10 diagonal is planned with that triangulation.
  var flipVal = packFace(api, 153, 255, 153, 255) // faceFlip => c01-c10
  var flipSlice = ndarray(new Int32Array(1), [1, 1])
  flipSlice.data[0] = flipVal
  var flipGrid = exact.buildGrid(flipSlice, 1, 1, 2, null)
  cc(exact.rectCompatible(flipGrid, false, 0, 0, 1, 1) === true &&
        exact.rectCompatible(flipGrid, true, 0, 0, 1, 1) === false,
        "only the matching diagonal is compatible")
  cc(exact.planSlice(flipGrid)[0].diag === 1,
        "planner selects the flipAO triangulation when required")

  return calls
}

// ---------------------------------------------------------------------------
// Group 3: compatibility, boundaries, formats and geometry
// ---------------------------------------------------------------------------

function compatibility(api, check) {
  var calls = 0
  var _check = check
  function cc(cond, msg) { _check(cond, msg); calls++ }

  //1. Unit face: the diagonal the reference uses is compatible, the other
  //   diagonal is rejected (the overlay crossing is inside the cell).
  var val = packFace(api, 153, 255, 204, 255)
  var c = corners(api, val)
  var one = ndarray(new Int32Array(1), [1, 1])
  one.data[0] = val
  var g1 = exact.buildGrid(one, 1, 1, 2, null)
  var usesFlip = faceFlipOf(c)
  cc(exact.rectCompatible(g1, !usesFlip, 0, 0, 1, 1) === true,
        "reference diagonal compatible on a unit face")
  cc(exact.rectCompatible(g1, usesFlip, 0, 0, 1, 1) === false,
        "opposite diagonal rejected on a unit face")

  //2. Legacy distortion counterexample.  Feed a same-mask U-gradient strip
  //   through the real post-stencil legacy pipeline (greedy mesher +
  //   append): it emits one stretched rectangle whose interior value differs
  //   from the unit-face reference, while exact compatibility rejects it.
  var gradVal = packFace(api, 153, 204, 255, 153)
  var W = 4
  var grad = ndarray(new Int32Array(W), [W, 1])
  for(var i = 0; i < W; ++i) grad.data[i] = gradVal
  var builder = new api.MeshBuilder()
  builder.d = 2; builder.u = 1; builder.v = 0; builder.z = 0
  builder.voxelSideTextureIDs = null
  var mesher = compileMesher({
    order: [1, 0],
    append: api.MeshBuilder.prototype.append.bind(builder)
  })
  mesher(grad)
  var legacyQuad = builder.buffer.subarray(0, builder.ptr)
  cc(legacyQuad.length === 48, "legacy greedy path merges the same-mask strip")
  var q = parseQuads(legacyQuad)[0]
  var stretched = quadAt(q, 0.5, 0.5)
  var reference = refAt(api, gradVal, 0.5, 0.5)
  cc(Math.abs(stretched - reference) > 1e-9,
        "stretched legacy field (" + stretched + ") distorts reference (" + reference + ")")
  var gGrad = exact.buildGrid(grad, W, 1, 2, null)
  cc(exact.rectCompatible(gGrad, true, 0, 0, W, 1) === false &&
        exact.rectCompatible(gGrad, false, 0, 0, W, 1) === false &&
        exact.rectCompatible(gGrad, !faceFlipOf(corners(api, gradVal)), 0, 0, 1, 1) === true,
        "exact check rejects the strip but accepts each unit face")

  //3. Texture boundary: same AO, different per-voxel texture id must split.
  var tA = packFace(api, 255, 255, 255, 255, 1)
  var tB = packFace(api, 255, 255, 255, 255, 2)
  var texSlice = ndarray(new Int32Array(2), [2, 1])
  texSlice.data[0] = tA; texSlice.data[1] = tB
  var texGrid = exact.buildGrid(texSlice, 2, 1, 2, null)
  cc(exact.planSlice(texGrid).length === 2, "texture boundary prevents merging")

  //4. Transparent voxels: the mesh still covers every visible face once and
  //   keeps an exact field.
  var scene = randomScene(api, 777, 7, 0.45, true)
  var bytes = api.exactAO(scene)
  cc(bytes instanceof Uint8Array && bytes.length % 8 === 0,
        "transparent-voxel scene produces an 8-byte vertex stream")
  var ao = faceGrids(api, scene)
  var quads = parseQuads(bytes)
  var visible = 0, covered = 0, worst = 0
  var samples = [[0.3, 0.4], [0.7, 0.6]]
  for(var d = 0; d < 3; ++d) {
    var u = (d + 1) % 3, v = (d + 2) % 3, dims = [7, 7, 7]
    for(var si = 0; si < dims[d]; ++si) {
      var sl = ao[d].transpose(d, u, v).pick(si)
      var here = quads.filter(function(qq) { return qq.d === d && qq.z === si })
      var mark = new Int32Array(dims[u] * dims[v])
      here.forEach(function(qq) {
        for(var r = qq.v0; r < qq.v1; ++r)
          for(var qq2 = qq.u0; qq2 < qq.u1; ++qq2) mark[r * dims[u] + qq2]++
      })
      for(var r3 = 0; r3 < dims[v]; ++r3)
        for(var q3 = 0; q3 < dims[u]; ++q3) {
          if(sl.get(q3, r3)) {
            visible++
            if(mark[r3 * dims[u] + q3] === 1) covered++
            var fv = sl.get(q3, r3)
            var owner = here.filter(function(hh) {
              return q3 >= hh.u0 && q3 < hh.u1 && r3 >= hh.v0 && r3 < hh.v1
            })[0]
            samples.forEach(function(pt) {
              worst = Math.max(worst,
                Math.abs(quadAt(owner, q3 + pt[0], r3 + pt[1]) -
                         refAt(api, fv, pt[0], pt[1])))
            })
          }
        }
    }
  }
  cc(visible > 0 && covered === visible && worst <= 1e-9,
        "transparent scene: full exact coverage, no missing or duplicated faces")

  //5. Negative strides: a stepped view matches the physically mirrored array.
  var base = randomScene(api, 4242, 6, 0.4, false)
  var S = 6
  function mirrored(signs) {
    var out = ndarray(new Uint16Array((S + 2) ** 3), [S + 2, S + 2, S + 2])
    for(var i = 0; i <= S + 1; ++i)
      for(var j = 0; j <= S + 1; ++j)
        for(var k = 0; k <= S + 1; ++k) {
          var mi = signs[0] < 0 ? S + 1 - i : i
          var mj = signs[1] < 0 ? S + 1 - j : j
          var mk = signs[2] < 0 ? S + 1 - k : k
          out.set(mi, mj, mk, base.get(i, j, k))
        }
    return out
  }
  var strideOk = true
  ;[[-1, 1, 1], [1, -1, 1], [1, 1, -1], [-1, -1, -1]].forEach(function(signs) {
    var got = api.exactAO(base.step(signs[0], signs[1], signs[2]))
    var want = api.exactAO(mirrored(signs))
    if(!got || got.length !== want.length ||
       Buffer.compare(Buffer.from(got), Buffer.from(want)) !== 0) strideOk = false
  })
  cc(strideOk, "negative-stride views match physically mirrored arrays")

  //6. Repeated calls are stable and do not corrupt pooled buffers.
  var repScene = randomScene(api, 909, 7, 0.5, false)
  var first = api.exactAO(repScene)
  var stable = true
  for(var t = 0; t < 5; ++t) {
    api(repScene)
    var again = api.exactAO(repScene)
    if(Buffer.compare(Buffer.from(first), Buffer.from(again)) !== 0) stable = false
  }
  cc(stable, "repeated exactAO calls return identical bytes")

  //7. Coordinate range guard.
  var tooBig = ndarray(new Uint16Array(258 * 4 * 4), [258, 4, 4])
  var threw = false
  try { api.exactAO(tooBig) } catch(e) { threw = e instanceof RangeError }
  cc(threw, "out-of-byte-range coordinates throw RangeError")
  var maxOk = ndarray(new Uint16Array(257 * 4 * 4), [257, 4, 4])
  maxOk.set(255, 1, 1, api.OPAQUE_BIT | 1)
  cc(api.exactAO(maxOk) instanceof Uint8Array, "max byte coordinate is accepted")

  //8. Geometry: uniform winding per facing and correct normal sign/encoding.
  var geomScene = randomScene(api, 31337, 8, 0.4, true)
  var geomBytes = api.exactAO(geomScene)
  var geomQuads = parseQuads(geomBytes)
  var ax = ["x", "y", "z"]
  var windingOk = true, normalOk = true
  ;[129, 127].forEach(function(sign) {
    var refs = geomQuads.filter(function(qq) {
      return qq.verts[0]["n" + ax[qq.d]] === sign
    })
    if(!refs.length) return
    var acc = 0
    refs.forEach(function(qq) {
      var A = qq.verts[0], B = qq.verts[1], C = qq.verts[2]
      acc += Math.sign(
        (B[ax[qq.u]] - A[ax[qq.u]]) * (C[ax[qq.v]] - A[ax[qq.v]]) -
        (B[ax[qq.v]] - A[ax[qq.v]]) * (C[ax[qq.u]] - A[ax[qq.u]]))
      var nb = [A.nx, A.ny, A.nz]
      if(nb[qq.d] !== sign || nb[(qq.d + 1) % 3] !== 128 || nb[(qq.d + 2) % 3] !== 128) {
        normalOk = false
      }
    })
    if(Math.abs(acc) !== refs.length) windingOk = false
  })
  cc(windingOk, "all faces have consistent, non-reversed winding")
  cc(normalOk, "normal bytes use the legacy 127/128/129 encoding")

  return calls
}

module.exports = {
  field: field,
  planning: planning,
  compatibility: compatibility
}
