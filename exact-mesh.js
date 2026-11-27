"use strict"

// Exact ambient-occlusion mesher.
//
// The legacy path (mesh.js) treats the four corner AO bits of a unit face as
// an ordinary face label: any two faces with the same label get stretched into
// one big rectangle.  The renderer, however, interpolates the AO intensity
// linearly inside each triangle, so four equal corner codes do not imply that
// the interior of a stretched rectangle keeps the same shading as the unit
// faces it replaced.
//
// This path keeps the full per-unit-face occlusion field.  A unit face is
// described by four corner intensities (looked up through AO_TABLE, kept in
// double precision) and the diagonal chosen by the same flipAO rule the legacy
// append() uses.  The reference field is the continuous piecewise-affine field
// defined by those unit triangles with barycentric interpolation.  A candidate
// rectangle is only merged when, for one of its diagonals, the two big
// triangles reproduce that reference field exactly over the whole rectangle
// (tolerance EPS), including the regions created where the new diagonal crosses
// the original unit-triangle edges.

var AO_TABLE = [0, 153, 204, 255]
var EPS = 1e-9

var OPAQUE_BIT    = (1 << 15)
var VOXEL_MASK    = (1 << 16) - 1
var VERTEX_SIZE   = 8

// Reused verbatim from mesh.js so the reference field is built from the exact
// same occlusion numbers as the legacy path.
function vertexAO(s1, s2, c) {
  if (s1 && s2) {
    return 1
  }
  return 3 - (s1 + s2 + c)
}

function facetAO(a00, a01, a02,
                 a10,      a12,
                 a20, a21, a22) {
  var s00 = (a00 & OPAQUE_BIT) ? 1 : 0
    , s01 = (a01 & OPAQUE_BIT) ? 1 : 0
    , s02 = (a02 & OPAQUE_BIT) ? 1 : 0
    , s10 = (a10 & OPAQUE_BIT) ? 1 : 0
    , s12 = (a12 & OPAQUE_BIT) ? 1 : 0
    , s20 = (a20 & OPAQUE_BIT) ? 1 : 0
    , s21 = (a21 & OPAQUE_BIT) ? 1 : 0
    , s22 = (a22 & OPAQUE_BIT) ? 1 : 0
  return [
    vertexAO(s10, s01, s00),
    vertexAO(s01, s12, s02),
    vertexAO(s12, s21, s22),
    vertexAO(s21, s10, s20)
  ]
}

function voxelTexture(voxel, side, voxelSideTextureIDs) {
  return voxelSideTextureIDs ? voxelSideTextureIDs.get(voxel & 0xff, side) : voxel & 0xff
}

// Decide the diagonal exactly as append() does, given the four AO_TABLE
// intensities in mesh.js variable order (a00, a10, a11, a01).  This is the
// legacy flipAO flag.
function legacyFlipAO(a00, a10, a11, a01) {
  var flipAO = a00 + a11 < a10 + a01
  if (a00 + a11 === a10 + a01) {
    flipAO = Math.max(a00, a11) < Math.max(a10, a01)
  }
  return flipAO
}

// Physical in-plane corner values of a unit face.  The facetAO codes follow
// mesh.js naming, but the stencil's second subscript runs along the first
// in-plane axis u=(d+1)%3 and the third along v=(d+2)%3.  Tracking the actual
// sample positions gives:
//
//   A = code0 (a00) corner (u-, v-)
//   B = code1 (a10) corner (u+, v-)   [mesh's "a10" variable]
//   C = code2 (a11) corner (u+, v+)
//   D = code3 (a01) corner (u-, v+)   [mesh's "a01" variable]
//
// A legacy flipAO of false selects the A-C ((0,0)-(1,1)) diagonal; true
// selects the B-D ((1,0)-(0,1)) diagonal.
function faceCorners(f) {
  return { A: f.cA, B: f.cB, C: f.cC, D: f.cD, diagAC: f.diagAC }
}

//
// Reference-field construction
//
function buildFaces(array, nx, ny, nz, textureIDs) {
  var stride = array.stride
  var offset = array.offset
  var data = array.data
  var generic = array.dtype === "generic"

  function cellAt(p) {
    // p is a core index [cx, cy, cz]; the padded voxel is at p+1.
    var idx = offset + stride[0] * (p[0] + 1)
                    + stride[1] * (p[1] + 1)
                    + stride[2] * (p[2] + 1)
    return generic ? data.get(idx) : data[idx]
  }

  var dims = [nx, ny, nz]
  // faces[d] is indexed by boundary slice b (0..dims[d]) plus the in-plane
  // coordinates: b*nu*nv + v*nu + u.  A flip face owned by solid core cell i
  // lives at boundary b=i; a non-flip face at b=i+1, matching the coordinate
  // append() writes.
  var faces = [
    new Array((nx + 1) * ny * nz),
    new Array((ny + 1) * nx * nz),
    new Array((nz + 1) * nx * ny)
  ]

  for (var d = 0; d < 3; ++d) {
    var u = (d + 1) % 3
    var v = (d + 2) % 3
    var nu = dims[u]
    var nv = dims[v]
    var nd_ = dims[d]
    var bucket = faces[d]

    for (var i = 0; i < nd_; ++i) {
      for (var vv = 0; vv < nv; ++vv) {
        for (var uu = 0; uu < nu; ++uu) {
          // scanCell plays the role of v111 in generateSurfaceVoxel.
          var scanCell = [0, 0, 0]
          scanCell[d] = i
          scanCell[u] = uu
          scanCell[v] = vv

          var back = scanCell.slice()
          back[d] = i - 1

          // Visibility rule identical to generateSurfaceVoxel().  Note the
          // first branch sets FLIP_BIT: the face on the low side of solid cell
          // v111 gets the minus normal.  Both branches emit at boundary i,
          // since the second branch's owner is cell i-1 whose high side is i.
          var v111 = cellAt(scanCell)
          var v011 = cellAt(back)
          var t0 = !(v011 & OPAQUE_BIT)
          var t1 = !(v111 & OPAQUE_BIT)
          var flip
          if (v111 && (!v011 || (t0 && !t1))) {
            flip = true   // FLIP_BIT set, minus normal, side d+3
          } else if (v011 && (!v111 || (t1 && !t0))) {
            flip = false  // no FLIP_BIT, plus normal, side d
          } else {
            continue
          }
          var owner = flip ? v111 : v011
          var boundary = i

          // facetAO samples the 3x3 plane on the air side of the face:
          // plane i-1 (the v0xx stencil set) for a flip face, plane i (the
          // v1xx set) for a non-flip face.
          var airD = flip ? i - 1 : i
          function sv(du, dv) {
            var q = scanCell.slice()
            q[d] = airD
            q[u] += du
            q[v] += dv
            return cellAt(q)
          }

          var codes = facetAO(
            sv(-1, -1), sv(0, -1), sv(1, -1),
            sv(-1, 0),               sv(1, 0),
            sv(-1, 1),  sv(0, 1),    sv(1, 1))

          var a00 = AO_TABLE[codes[0]]
          var a10 = AO_TABLE[codes[1]]
          var a11 = AO_TABLE[codes[2]]
          var a01 = AO_TABLE[codes[3]]

          var side = d + (flip ? 3 : 0)
          var tex = voxelTexture(owner & VOXEL_MASK, side, textureIDs)

          var lf = legacyFlipAO(a00, a10, a11, a01)
          bucket[boundary * nu * nv + vv * nu + uu] = {
            u: uu,
            v: vv,
            flip: flip,
            tex: tex,
            // Raw AO_TABLE intensities in mesh.js variable order.
            a00: a00, a10: a10, a11: a11, a01: a01,
            // Legacy flipAO decision; reference diagonal A-C when false.
            legacyFlip: lf,
            diagAC: !lf,
            // Precomputed physical corner intensities.
            cA: a00, cB: a10, cC: a11, cD: a01,
            constant: a00 === a10 && a10 === a11 && a11 === a01
          }
        }
      }
    }
  }
  return faces
}

//
// Reference-field evaluation inside one unit cell.  (gu, gv) are local
// coordinates in [0,1]^2 along the physical u and v axes.
//
function refInsideCell(f, gu, gv) {
  if (f.diagAC) {
    // Diagonal A-C: triangle A-B-C where gv<=gu, A-C-D otherwise.
    if (gv <= gu) {
      return f.cA + (f.cB - f.cA) * gu + (f.cC - f.cB) * gv
    }
    return f.cA + (f.cC - f.cD) * gu + (f.cD - f.cA) * gv
  }
  // Diagonal B-D: triangle A-B-D where gu+gv<=1, B-C-D otherwise.
  if (gu + gv <= 1) {
    return f.cA + (f.cB - f.cA) * gu + (f.cD - f.cA) * gv
  }
  return f.cB * (1 - gv) + f.cC * (gu + gv - 1) + f.cD * (1 - gu)
}

// Reference value at any point of a unit cell, dispatching on grid edges.
function referenceAt(f, gu, gv) {
  if (gv <= EPS) return f.cA + (f.cB - f.cA) * gu          // bottom edge
  if (gv >= 1 - EPS) return f.cD + (f.cC - f.cD) * gu      // top edge
  if (gu <= EPS) return f.cA + (f.cD - f.cA) * gv          // left edge
  if (gu >= 1 - EPS) return f.cB + (f.cC - f.cB) * gv      // right edge
  return refInsideCell(f, gu, gv)
}

// Reference value at an integer grid corner of the rectangle.
function refAtGrid(rect, gu, gv) {
  var g = rect.grid, nu = rect.nu, u0 = rect.u0, v0 = rect.v0
  var uu = gu, vv = gv
  if (uu < rect.w && vv < rect.h) {
    return g[(v0 + vv) * nu + (u0 + uu)].cA
  }
  if (uu > 0 && vv < rect.h) {
    return g[(v0 + vv) * nu + (u0 + uu - 1)].cB
  }
  if (uu < rect.w && vv > 0) {
    return g[(v0 + vv - 1) * nu + (u0 + uu)].cD
  }
  return g[(v0 + vv - 1) * nu + (u0 + uu - 1)].cC
}

// Test whether the two big triangles of the rectangle (physical corner values
// rect.A..D) reproduce the reference field.  bigDiagAC=true picks the A-C
// ((0,0)-(w,h)) diagonal, false the B-D ((w,0)-(0,h)) diagonal.
function diagonalExact(rect, bigDiagAC) {
  var wd = rect.w, hd = rect.h
  var u0 = rect.u0, v0 = rect.v0, nu = rect.nu
  var A = rect.A, B = rect.B, C = rect.C, D = rect.D
  var m = hd / wd

  function bigAt(gu, gv) {
    // Normalize to unit rectangle coordinates; gu spans [0,wd], gv [0,hd].
    var x = gu / wd
    var y = gv / hd
    if (bigDiagAC) {
      if (y <= x) {
        // Triangle A-B-C: bary weights (1-x, x-y, y).
        return A * (1 - x) + B * (x - y) + C * y
      }
      // Triangle A-C-D: weights (1-y, x, y-x).
      return A * (1 - y) + C * x + D * (y - x)
    }
    if (x + y <= 1) {
      // Triangle A-B-D: weights (1-x-y, x, y).
      return A * (1 - x - y) + B * x + D * y
    }
    // Triangle B-C-D: weights (1-y, x+y-1, 1-x).
    return B * (1 - y) + C * (x + y - 1) + D * (1 - x)
  }
  function ok(gu, gv, f, cellU, cellV) {
    var refv = referenceAt(f, gu - cellU, gv - cellV)
    if (Number.isNaN(refv)) return false
    return Math.abs(refv - bigAt(gu, gv)) <= EPS
  }
  function strictlyInside(gu, gv, cellU, cellV) {
    return gu > cellU + EPS && gu < cellU + 1 - EPS &&
           gv > cellV + EPS && gv < cellV + 1 - EPS
  }

  // 1. Every integer grid corner of the rectangle.
  for (var iv = 0; iv <= hd; ++iv) {
    for (var iu = 0; iu <= wd; ++iu) {
      if (Math.abs(refAtGrid(rect, iu, iv) - bigAt(iu, iv)) > EPS) {
        return false
      }
    }
  }

  // 2. Non-grid overlay vertices inside each unit cell: the big diagonal's
  //    crossings with cell-edge interiors and, when the cell's own diagonal
  //    differs, its interior crossing with that diagonal.  No allocations.
  for (var cellV = 0; cellV < hd; ++cellV) {
    for (var cellU = 0; cellU < wd; ++cellU) {
      var f = rect.grid[(v0 + cellV) * nu + (u0 + cellU)]

      var gx, gy
      // Left and right edge interiors (gu fixed at a cell boundary).
      gx = cellU
      gy = bigDiagAC ? m * cellU : m * (wd - cellU)
      if (gy > cellV + EPS && gy < cellV + 1 - EPS &&
          !ok(gx, gy, f, cellU, cellV)) return false
      gx = cellU + 1
      gy = bigDiagAC ? m * (cellU + 1) : m * (wd - cellU - 1)
      if (gy > cellV + EPS && gy < cellV + 1 - EPS &&
          !ok(gx, gy, f, cellU, cellV)) return false
      // Bottom and top edge interiors (gv fixed at a cell boundary).
      gy = cellV
      gx = bigDiagAC ? cellV / m : wd - cellV / m
      if (gx > cellU + EPS && gx < cellU + 1 - EPS &&
          !ok(gx, gy, f, cellU, cellV)) return false
      gy = cellV + 1
      gx = bigDiagAC ? (cellV + 1) / m : wd - (cellV + 1) / m
      if (gx > cellU + EPS && gx < cellU + 1 - EPS &&
          !ok(gx, gy, f, cellU, cellV)) return false

      // Interior crossing of the big diagonal and the cell's own diagonal.
      if (f.diagAC !== bigDiagAC) {
        if (bigDiagAC) {
          // Cell B-D diagonal: gv = cellU+cellV+1-gu; big A-C: gv = m*gu.
          gx = (cellU + cellV + 1) / (m + 1)
          gy = m * gx
        } else {
          // Cell A-C diagonal: gv = gu-cellU+cellV; big B-D: gv=m(wd-gu).
          gx = (m * wd + cellU - cellV) / (m + 1)
          gy = m * (wd - gx)
        }
        if (strictlyInside(gx, gy, cellU, cellV) &&
            !ok(gx, gy, f, cellU, cellV)) return false
      }
    }
  }

  return true
}

// A rectangle is mergeable when all its unit faces share normal orientation
// and texture, the region is complete, and at least one diagonal reproduces the
// reference field.  Returns the chosen big diagonal (true = A-C) or null.
// When both diagonals are legal the anchor unit face's preference wins.
function evaluateRect(faceGrid, nu, nv, u0, v0, w, h) {
  var first = faceGrid[v0 * nu + u0]
  if (!first) return null
  var uniform = first.constant
  var flat = first.cA
  for (var vv = 0; vv < h; ++vv) {
    var rowBase = (v0 + vv) * nu
    for (var uu = 0; uu < w; ++uu) {
      var f = faceGrid[rowBase + u0 + uu]
      if (!f) return null
      if (f.flip !== first.flip || f.tex !== first.tex) return null
      if (!f.constant || f.cA !== flat) uniform = false
    }
  }

  // Uniform flat region: both diagonals reproduce the constant field.
  if (uniform) return first.diagAC

  var rect = {
    grid: faceGrid, nu: nu, u0: u0, v0: v0, w: w, h: h,
    // Physical rectangle corners assembled from the owning unit faces.
    A: first.cA,
    B: faceGrid[v0 * nu + (u0 + w - 1)].cB,
    C: faceGrid[(v0 + h - 1) * nu + (u0 + w - 1)].cC,
    D: faceGrid[(v0 + h - 1) * nu + u0].cD
  }

  // A single non-constant unit face is exact on its own reference diagonal.
  if (w === 1 && h === 1) return first.diagAC

  var acOK = diagonalExact(rect, true)
  var bdOK = diagonalExact(rect, false)
  if (!acOK && !bdOK) return null
  if (acOK && bdOK) {
    // Existing AO preference: flipAO over the rectangle's four corners, exactly
    // as append() computes it for a merged face label.
    return !legacyFlipAO(rect.A, rect.B, rect.C, rect.D)
  }
  return acOK
}

//
// Fixed merge order: boundary slices along the normal axis, then within each
// slice the second in-plane coordinate first, then the first.  At the first
// uncovered unit face, enumerate every uncovered legal rectangle anchored
// there, take the largest area, ties broken by the larger span along the first
// in-plane axis (u).  Both diagonals are evaluated for every candidate.
//
// Structural run bounds keep the enumeration small: a rectangle can never cross
// a missing, already-covered, differently-oriented or differently-textured
// face, so the candidate widths/heights are limited to the contiguous runs.
//
function planSlice(faceGrid, nu, nv) {
  var covered = new Uint8Array(nu * nv)
  var rects = []

  for (var vv = 0; vv < nv; ++vv) {
    for (var uu = 0; uu < nu; ++uu) {
      var anchorIdx = vv * nu + uu
      if (covered[anchorIdx] || !faceGrid[anchorIdx]) continue
      var anchor = faceGrid[anchorIdx]

      // Vertical structural run at column uu (consecutive compatible rows).
      var maxH = 0
      for (var r = vv; r < nv; ++r) {
        var cf = faceGrid[r * nu + uu]
        if (!cf || covered[r * nu + uu] ||
            cf.flip !== anchor.flip || cf.tex !== anchor.tex) break
        maxH++
      }

      // Per-row horizontal runs; the tightest run bounds every candidate.
      var rowRuns = new Array(maxH)
      var boxW = nu - uu
      for (var hh = 0; hh < maxH; ++hh) {
        var row = vv + hh
        var runW = 0
        for (var c = uu; c < nu; ++c) {
          var rf = faceGrid[row * nu + c]
          if (!rf || covered[row * nu + c] ||
              rf.flip !== anchor.flip || rf.tex !== anchor.tex) break
          runW++
        }
        rowRuns[hh] = runW
        if (runW < boxW) boxW = runW
      }

      // Uniform flat box: the whole structural box merges in one rectangle,
      // evaluated once instead of enumerating every sub-rectangle.
      if (anchor.constant) {
        var uniform = true
        for (var h2 = 0; h2 < maxH && uniform; ++h2) {
          var rb = (vv + h2) * nu
          for (var w2 = 0; w2 < boxW; ++w2) {
            var uf = faceGrid[rb + uu + w2]
            if (!uf.constant || uf.cA !== anchor.cA) { uniform = false; break }
          }
        }
        if (uniform) {
          var whole = { u0: uu, v0: vv, w: boxW, h: maxH,
                        diagAC: anchor.diagAC, area: boxW * maxH }
          for (var cy0 = 0; cy0 < whole.h; ++cy0)
            for (var cx0 = 0; cx0 < whole.w; ++cx0)
              covered[(vv + cy0) * nu + (uu + cx0)] = 1
          rects.push(whole)
          continue
        }
      }

      var best = null
      var minW = nu - uu
      for (var h = 1; h <= maxH; ++h) {
        if (rowRuns[h - 1] < minW) minW = rowRuns[h - 1]
        for (var w = 1; w <= minW; ++w) {
          var diagAC = evaluateRect(faceGrid, nu, nv, uu, vv, w, h)
          if (diagAC === null) continue
          var area = w * h
          if (!best ||
              area > best.area ||
              (area === best.area && w > best.w)) {
            best = { u0: uu, v0: vv, w: w, h: h, diagAC: diagAC, area: area }
          }
        }
      }

      if (!best) {
        // A single unit face is always representable by its own diagonal.
        best = { u0: uu, v0: vv, w: 1, h: 1, diagAC: anchor.diagAC, area: 1 }
      }

      for (var cy = 0; cy < best.h; ++cy)
        for (var cx = 0; cx < best.w; ++cx)
          covered[(best.v0 + cy) * nu + (best.u0 + cx)] = 1
      rects.push(best)
    }
  }
  return rects
}

//
// Vertex emission, matching MeshBuilder.append() winding and encoding.  The
// corner sequences are translated into physical (u, v) coordinates; the normal
// byte and texture slot are identical to the legacy path.
//
function emitRect(out, d, u, v, boundary, rect, faceGrid, nu) {
  var anchor = faceGrid[rect.v0 * nu + rect.u0]
  var flip = anchor.flip

  var u0 = rect.u0, v0 = rect.v0
  var u1 = rect.u0 + rect.w, v1 = rect.v0 + rect.h
  var A = faceCorners(faceGrid[rect.v0 * nu + rect.u0]).A
  var B = faceCorners(faceGrid[rect.v0 * nu + (rect.u0 + rect.w - 1)]).B
  var C = faceCorners(faceGrid[(rect.v0 + rect.h - 1) * nu +
                               (rect.u0 + rect.w - 1)]).C
  var D = faceCorners(faceGrid[(rect.v0 + rect.h - 1) * nu + rect.u0]).D
  var tex = anchor.tex

  var nx = 128, ny = 128, nz = 128
  var sign = flip ? 127 : 129
  if (d === 0) nx = sign
  else if (d === 1) ny = sign
  else nz = sign

  function vert(point) {
    var xyz = [0, 0, 0]
    xyz[u] = point[0]
    xyz[v] = point[1]
    xyz[d] = boundary
    out.push(xyz[0], xyz[1], xyz[2], point[2], nx, ny, nz, tex)
  }
  function emit6(p0, p1, p2, p3, p4, p5) {
    vert(p0); vert(p1); vert(p2); vert(p3); vert(p4); vert(p5)
  }

  var PA = [u0, v0, A]
  var PB = [u1, v0, B]
  var PC = [u1, v1, C]
  var PD = [u0, v1, D]

  // The six-vertex orders reproduce MeshBuilder.append() exactly, including
  // winding.  append() writes lo_x/hi_x along the physical v axis and
  // lo_y/hi_y along the physical u axis, which gives:
  //   (lo_x,lo_y,a00)=PA  (lo_x,hi_y,a01)=PB
  //   (hi_x,lo_y,a10)=PD  (hi_x,hi_y,a11)=PC
  if (rect.diagAC) {
    if (!flip) {
      // append() flipAO=false, flip=false
      emit6(PA, PB, PC, PC, PD, PA)
    } else {
      // append() flipAO=false, flip=true
      emit6(PB, PA, PC, PD, PC, PA)
    }
  } else {
    if (!flip) {
      // append() flipAO=true, flip=false
      emit6(PA, PB, PD, PC, PD, PB)
    } else {
      // append() flipAO=true, flip=true
      emit6(PA, PD, PB, PC, PB, PD)
    }
  }
}

function exactAO(array, voxelSideTextureIDs) {
  if (array.dimension !== 3) {
    return null
  }
  var nx = array.shape[0] - 2
  var ny = array.shape[1] - 2
  var nz = array.shape[2] - 2
  if (nx <= 0 || ny <= 0 || nz <= 0) {
    return null
  }
  // Coordinates are still written into single bytes.
  if (nx > 255 || ny > 255 || nz > 255) {
    throw new RangeError("exactAO: core extent must fit in one byte per axis")
  }

  var faces = buildFaces(array, nx, ny, nz, voxelSideTextureIDs || null)
  var dims = [nx, ny, nz]
  var out = []

  for (var d = 0; d < 3; ++d) {
    var u = (d + 1) % 3
    var v = (d + 2) % 3
    var nu = dims[u]
    var nv = dims[v]
    var nBoundary = dims[d] + 1
    var bucket = faces[d]

    for (var b = 0; b < nBoundary; ++b) {
      var base = b * nu * nv
      var sliceGrid = bucket.slice(base, base + nu * nv)
      var rects = planSlice(sliceGrid, nu, nv)
      for (var r = 0; r < rects.length; ++r) {
        emitRect(out, d, u, v, b, rects[r], sliceGrid, nu)
      }
    }
  }

  if (out.length === 0) {
    return null
  }
  return new Uint8Array(out)
}

module.exports = exactAO
module.exports.AO_TABLE = AO_TABLE
module.exports.EPS = EPS
module.exports.VERTEX_SIZE = VERTEX_SIZE
// Exposed for the regression tests so they can reconstruct the reference
// field independently of the merge planning.
module.exports.buildFaces = buildFaces
module.exports.planSlice = planSlice
module.exports.evaluateRect = evaluateRect
module.exports.diagonalExact = diagonalExact
module.exports.faceCorners = faceCorners
module.exports.legacyFlipAO = legacyFlipAO
module.exports.refInsideCell = refInsideCell
