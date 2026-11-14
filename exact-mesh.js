"use strict"

//Exact AO mesher.
//
//The legacy path (mesh.js) treats the four corner AO mask of a unit face as
//an ordinary face label: any two faces sharing the same packed value get
//stretched into one rectangle.  Four equal corners do not however imply that
//the piecewise affine field (two triangles per unit face, barycentric
//interpolation) stays identical after stretching.
//
//This path keeps the per-unit-face occlusion field.  A rectangle of unit
//faces is only merged when the two big triangles of one of its diagonals
//reproduce the reference field exactly over the whole rectangle, including
//the regions formed where the new diagonal crosses the old unit diagonals.

var ndarray = require("ndarray")
var pool = require("typedarray-pool")

var mesh = require("./mesh.js")

var VOXEL_MASK    = mesh.VOXEL_MASK
var FLIP_BIT      = mesh.FLIP_BIT
var AO_SHIFT      = mesh.AO_SHIFT
var AO_BITS       = mesh.AO_BITS
var AO_MASK       = mesh.AO_MASK
var AO_TABLE      = mesh.AO_TABLE
var VERTEX_SIZE   = mesh.VERTEX_SIZE
var MeshBuilder   = mesh.MeshBuilder
var voxelTexture  = mesh.voxelTexture

var EPS = 1e-9

//Maximum coordinate that the legacy byte vertex format can express.
var MAX_COORD = 255

//
// Reference field helpers
//
// A unit face cell indexed (q, r) inside a transposed slice has four physical
// corner intensities (double precision AO_TABLE values):
//
//   c00 at (q,   r)     c10 at (q+1, r)
//   c01 at (q,   r+1)   c11 at (q+1, r+1)
//
// q runs along slice axis u=(d+1)%3 and r along v=(d+2)%3.  The packed slot
// frame used by MeshBuilder.append is the transpose of this layout (see
// buildGrid), but the opposite-corner sums below are invariant under that
// transpose, so faceFlipAO returns exactly append's flipAO decision.
//

function aoIntensity(val, slot) {
  return AO_TABLE[(val >>> (AO_SHIFT + slot * AO_BITS)) & AO_MASK]
}

//Same rule as append's flipAO, evaluated from the physical-frame corner
//intensities.  true selects the c01-c10 diagonal (append's flipAO branch).
function faceFlipAO(c00, c01, c10, c11) {
  var flipAO = c00 + c11 < c10 + c01
  if(c00 + c11 === c10 + c01) {
    flipAO = Math.max(c00, c11) < Math.max(c10, c01)
  }
  return flipAO
}

//
// Rectangle compatibility
//
// The reference field is a triangulation of the rectangle where every unit
// cell carries its own diagonal.  A candidate diagonal cuts the rectangle
// into two big triangles.  The overlay of both triangulations is a planar
// straight-line subdivision: every overlay vertex lies either on the unit
// lattice (grid corners) or on a unit-grid edge where the big diagonal
// crosses it.  On each overlay polygon both fields are affine, so matching at
// every overlay vertex proves equality of the two continuous piecewise
// affine fields over the whole rectangle -- no point sampling involved.
//

function rectCompatible(grid, flip, q0, r0, q1, r1) {
  var w = q1 - q0
  var h = r1 - r0

  //Big-triangle affine coefficients.  Both diagonals share the interpolated
  //intensities at the four rectangle corners.
  var v00 = grid.get(q0, r0).c00
  var v10 = grid.get(q1 - 1, r0).c10
  var v01 = grid.get(q0, r1 - 1).c01
  var v11 = grid.get(q1 - 1, r1 - 1).c11

  //Reference intensity of the big triangles at global point (x, y).
  //nx/ny normalize the rectangle to the unit square.
  function bigValue(x, y) {
    var nx = x / w
    var ny = y / h
    if(flip) {
      //Diagonal c00-c11: triangle (00,10,11) where nx>=ny, (00,11,01) above
      if(nx >= ny) {
        return v00 + (v10 - v00) * nx + (v11 - v10) * ny
      }
      return v00 + (v11 - v01) * nx + (v01 - v00) * ny
    }
    //Diagonal c01-c10: triangle (00,10,01) where nx+ny<=1, (10,11,01) above
    if(nx + ny <= 1) {
      return v00 + (v10 - v00) * nx + (v01 - v00) * ny
    }
    return (v10 + v01 - v11) + (v11 - v01) * nx + (v11 - v10) * ny
  }

  //1. Every unit cell corner against the big-triangle field.  Adjacent unit
  //   faces may carry different stencil intensities at a shared lattice
  //   vertex, so every incident corner of every cell is checked separately.
  for(var r = r0; r < r1; ++r) {
    for(var q = q0; q < q1; ++q) {
      var cell = grid.get(q, r)
      if(Math.abs(cell.c00 - bigValue(q - q0, r - r0)) > EPS) return false
      if(Math.abs(cell.c10 - bigValue(q + 1 - q0, r - r0)) > EPS) return false
      if(Math.abs(cell.c11 - bigValue(q + 1 - q0, r + 1 - r0)) > EPS) return false
      if(Math.abs(cell.c01 - bigValue(q - q0, r + 1 - r0)) > EPS) return false
    }
  }

  //2. Interior crossings of the big diagonal with unit grid lines.  Each
  //   crossed unit edge is shared by two unit cells whose endpoint
  //   intensities can differ; both edge interpolations are checked against
  //   the big-triangle value at the crossing.
  var g, sx, sy, t, qc, rc, cellL, cellR, refL, refR
  function edgeOk() {
    return Math.abs(refL - bigValue(sx, sy)) <= EPS &&
           Math.abs(refR - bigValue(sx, sy)) <= EPS
  }
  if(flip) {
    //Diagonal y = (h/w) x ; crossings at vertical lines x = 1 .. w-1 ...
    for(g = 1; g < w; ++g) {
      sx = g
      sy = h * g / w
      t = sy - Math.floor(sy)
      if(t !== 0) {
        qc = q0 + g
        rc = r0 + Math.floor(sy)
        cellR = grid.get(qc, rc)
        cellL = grid.get(qc - 1, rc)
        refR = cellR.c00 + (cellR.c01 - cellR.c00) * t
        refL = cellL.c10 + (cellL.c11 - cellL.c10) * t
        if(!edgeOk()) return false
      }
    }
    //... and horizontal lines y = 1 .. h-1.
    for(g = 1; g < h; ++g) {
      sy = g
      sx = w * g / h
      t = sx - Math.floor(sx)
      if(t !== 0) {
        qc = q0 + Math.floor(sx)
        rc = r0 + g
        cellL = grid.get(qc, rc)
        cellR = grid.get(qc, rc - 1)
        refL = cellL.c00 + (cellL.c10 - cellL.c00) * t
        refR = cellR.c01 + (cellR.c11 - cellR.c01) * t
        if(!edgeOk()) return false
      }
    }
  } else {
    //Anti-diagonal: x/w + y/h = 1
    for(g = 1; g < w; ++g) {
      sx = g
      sy = h * (1 - g / w)
      t = sy - Math.floor(sy)
      if(t !== 0) {
        qc = q0 + g
        rc = r0 + Math.floor(sy)
        cellR = grid.get(qc, rc)
        cellL = grid.get(qc - 1, rc)
        refR = cellR.c00 + (cellR.c01 - cellR.c00) * t
        refL = cellL.c10 + (cellL.c11 - cellL.c10) * t
        if(!edgeOk()) return false
      }
    }
    for(g = 1; g < h; ++g) {
      sy = g
      sx = w * (1 - g / h)
      t = sx - Math.floor(sx)
      if(t !== 0) {
        qc = q0 + Math.floor(sx)
        rc = r0 + g
        cellL = grid.get(qc, rc)
        cellR = grid.get(qc, rc - 1)
        refL = cellL.c00 + (cellL.c10 - cellL.c00) * t
        refR = cellR.c01 + (cellR.c11 - cellR.c01) * t
        if(!edgeOk()) return false
      }
    }
  }

  //Reference field of one unit cell at a local [0,1]^2 point, using the
  //cell's own diagonal.  The diagonal follows the flipAO rule, not the
  //FLIP_BIT facing flag (which only changes winding and material side).
  function cellRef(cell, tx, ty) {
    if(cell.faceFlip) {
      //c01-c10 diagonal: triangle (00,01,10) where tx+ty<=1
      if(tx + ty <= 1) {
        return cell.c00 + (cell.c10 - cell.c00) * tx + (cell.c01 - cell.c00) * ty
      }
      return (cell.c10 + cell.c01 - cell.c11) +
             (cell.c11 - cell.c01) * tx + (cell.c11 - cell.c10) * ty
    }
    //c00-c11 diagonal
    if(tx >= ty) {
      return cell.c00 + (cell.c10 - cell.c00) * tx + (cell.c11 - cell.c10) * ty
    }
    return cell.c00 + (cell.c11 - cell.c01) * tx + (cell.c01 - cell.c00) * ty
  }

  //3. Interior crossings of the big diagonal with the unit cell diagonals.
  //   These overlay vertices sit inside cells, not on grid edges, so the
  //   checks above never see them (this is also what makes a 1x1 cell
  //   reject the wrong diagonal).
  var alpha = h / w
  var q2, r2, cell2, beta, gamma, tx, ty
  for(r2 = r0; r2 < r1; ++r2) {
    for(q2 = q0; q2 < q1; ++q2) {
      cell2 = grid.get(q2, r2)
      if(flip) {
        //Big c00-c11 line in local coords: ty = alpha*tx + beta
        beta = alpha * (q2 - q0) - (r2 - r0)
        if(cell2.faceFlip) {
          //Unit c01-c10 diagonal: ty = 1 - tx
          tx = (1 - beta) / (1 + alpha)
          ty = 1 - tx
        } else {
          //Unit c00-c11 diagonal: ty = tx ; parallel when alpha === 1
          if(alpha === 1) continue
          tx = beta / (1 - alpha)
          ty = tx
        }
      } else {
        //Big c01-c10 anti-diagonal in local coords: ty = gamma - alpha*tx
        gamma = h - alpha * (q2 - q0) - (r2 - r0)
        if(cell2.faceFlip) {
          //Unit c01-c10 diagonal: ty = 1 - tx ; parallel when alpha === 1
          if(alpha === 1) continue
          tx = (1 - gamma) / (1 - alpha)
          ty = 1 - tx
        } else {
          //Unit c00-c11 diagonal: ty = tx
          tx = gamma / (1 + alpha)
          ty = tx
        }
      }
      if(tx > EPS && tx < 1 - EPS && ty > EPS && ty < 1 - EPS) {
        var ref2 = cellRef(cell2, tx, ty)
        if(Math.abs(ref2 - bigValue(q2 - q0 + tx, r2 - r0 + ty)) > EPS) {
          return false
        }
      }
    }
  }

  return true
}

//
// Slice grid construction
//

function buildGrid(aoSlice, shapeU, shapeV, d, voxelSideTextureIDs) {
  var cells = new Array(shapeU * shapeV)
  for(var r = 0; r < shapeV; ++r) {
    for(var q = 0; q < shapeU; ++q) {
      var val = aoSlice.get(q, r)
      if(!val) {
        cells[r * shapeU + q] = null
        continue
      }
      var flip = !!(val & FLIP_BIT)
      //Packed slot frame (append) is transposed relative to the grid frame:
      //slot 0 = grid (q, r), slot 1 = grid (q, r+1),
      //slot 2 = grid (q+1, r+1), slot 3 = grid (q+1, r).
      var s0 = aoIntensity(val, 0)
      var s1 = aoIntensity(val, 1)
      var s2 = aoIntensity(val, 2)
      var s3 = aoIntensity(val, 3)
      var c00 = s0
      var c01 = s1
      var c11 = s2
      var c10 = s3
      var side = d + (flip ? 3 : 0)
      var texId = voxelTexture(val & VOXEL_MASK, side, voxelSideTextureIDs)
      cells[r * shapeU + q] = {
        flip: flip,
        faceFlip: faceFlipAO(c00, c01, c10, c11),
        c00: c00,
        c01: c01,
        c10: c10,
        c11: c11,
        tex: texId
      }
    }
  }
  return {
    shapeU: shapeU,
    shapeV: shapeV,
    cells: cells,
    get: function(q, r) {
      return cells[r * shapeU + q]
    }
  }
}

//Textures and visibility must match across every cell of a candidate.
function cellMergesWith(grid, cell, q, r) {
  var other = grid.get(q, r)
  if(!other) return false
  if(other.tex !== cell.tex) return false
  //The FLIP_BIT selects the facing side of the voxel pair; packed AO values
  //with different flip bits describe faces on opposite sides, never the same
  //surface rectangle.
  if(other.flip !== cell.flip) return false
  return true
}

//
// Rectangle emission
//
// Mirrors MeshBuilder.append byte for byte (positions, normal encoding,
// material side, AO_TABLE intensity bytes).  Grid coordinates run along the
// transposed slice axes: q varies along u=(d+1)%3 and r along v=(d+2)%3,
// while append writes its x slot through builder.u=v and its y slot through
// builder.v=u.  The packed corner slots follow that same convention, so the
// grid's c01 (varies with r) maps to append's a10 slot and c10 (varies with
// q) maps to a01.  diag follows append's flipAO convention: 1 selects the
// flipAO triangulation (c01-c10 diagonal), 2 the default (c00-c11).
//

function emitRectangle(builder, q0, r0, q1, r1, facingFlip, diag, corners, texId) {
  var buffer = builder.buffer
  var ptr = builder.ptr >>> 0
  var z = builder.z | 0
  var u = builder.u | 0
  var v = builder.v | 0
  var d = builder.d | 0

  if(ptr + 6 * VERTEX_SIZE > buffer.length) {
    var grown = pool.mallocUint8(2 * buffer.length)
    grown.set(buffer)
    pool.freeUint8(buffer)
    buffer = grown
    builder.buffer = buffer
  }

  var a00 = corners.a00
  var a10 = corners.a01
  var a11 = corners.a11
  var a01 = corners.a10

  var nx = 128, ny = 128, nz = 128
  var sign = facingFlip ? 127 : 129
  if(d === 0) {
    nx = sign
  } else if(d === 1) {
    ny = sign
  } else {
    nz = sign
  }

  function vert(x, y, intensity) {
    buffer[ptr + u] = x
    buffer[ptr + v] = y
    buffer[ptr + d] = z
    buffer[ptr + 3] = intensity
    buffer[ptr + 4] = nx
    buffer[ptr + 5] = ny
    buffer[ptr + 6] = nz
    buffer[ptr + 7] = texId
    ptr += 8
  }

  var lo = r0, hi = r1, loY = q0, hiY = q1
  if(diag === 1) {
    if(!facingFlip) {
      vert(lo, loY, a00); vert(lo, hiY, a01); vert(hi, loY, a10)
      vert(hi, hiY, a11); vert(hi, loY, a10); vert(lo, hiY, a01)
    } else {
      vert(lo, loY, a00); vert(hi, loY, a10); vert(lo, hiY, a01)
      vert(hi, hiY, a11); vert(lo, hiY, a01); vert(hi, loY, a10)
    }
  } else {
    if(facingFlip) {
      vert(lo, hiY, a01); vert(lo, loY, a00); vert(hi, hiY, a11)
      vert(hi, loY, a10); vert(hi, hiY, a11); vert(lo, loY, a00)
    } else {
      vert(lo, loY, a00); vert(lo, hiY, a01); vert(hi, hiY, a11)
      vert(hi, hiY, a11); vert(hi, loY, a10); vert(lo, loY, a00)
    }
  }

  builder.ptr = ptr
}

//
// Rectangle planning
//
// Fixed traversal over one slice: second in-plane coordinate (r) first, then
// the first (q); from each uncovered visible seed cell enumerate every
// uncovered fully-visible rectangle with that lower-left corner and keep the
// largest exact one (larger area, tie broken toward the larger first-axis
// span).  Each returned rectangle records which diagonal reproduces the
// reference field, comparing BOTH diagonals independently.
//

function planSlice(grid) {
  var shapeU = grid.shapeU
  var shapeV = grid.shapeV
  var covered = new Uint8Array(shapeU * shapeV)
  var rects = []

  for(var r = 0; r < shapeV; ++r) {
    for(var q = 0; q < shapeU; ++q) {
      if(covered[r * shapeU + q]) continue
      var seed = grid.get(q, r)
      if(!seed) {
        covered[r * shapeU + q] = 1
        continue
      }

      var best = null
      var maxR = shapeV
      var maxQ = shapeU
      var rp
      for(rp = r; rp < maxR; ++rp) {
        var qp
        for(qp = q; qp < maxQ; ++qp) {
          if(covered[rp * shapeU + qp] || !cellMergesWith(grid, seed, qp, rp)) {
            break
          }
        }
        //A later row can never extend past an earlier row's limit,
        //otherwise the rectangle would contain a bad or missing cell.
        maxQ = qp
        if(maxQ === q) break

        for(qp = q; qp < maxQ; ++qp) {
          var spanU = qp + 1 - q
          var spanV = rp + 1 - r
          var area = spanU * spanV
          if(best && area < best.area) continue
          if(best && area === best.area && spanU <= best.spanU) continue

          var okFlip = rectCompatible(grid, true,  q, r, qp + 1, rp + 1)
          var okNorm = rectCompatible(grid, false, q, r, qp + 1, rp + 1)
          if(!okFlip && !okNorm) continue

          //Emitter diag follows append's flipAO convention:
          //  diag 1 => flipAO triangulation, diagonal c01-c10
          //  diag 2 => default triangulation, diagonal c00-c11
          //rectCompatible's flip argument names the opposite convention.
          var chosen
          if(okFlip && okNorm) {
            //Both diagonals reproduce the field: keep the same flipAO
            //preference rule that append applies to the four corners.
            var rc00 = grid.get(q, r).c00
            var rc10 = grid.get(qp, r).c10
            var rc11 = grid.get(qp, rp).c11
            var rc01 = grid.get(q, rp).c01
            var preferFlip = faceFlipAO(rc00, rc01, rc10, rc11)
            chosen = preferFlip ? 1 : 2
          } else if(okNorm) {
            chosen = 1
          } else {
            chosen = 2
          }

          best = {
            q0: q, r0: r, q1: qp + 1, r1: rp + 1,
            area: area, spanU: spanU, spanV: spanV,
            diag: chosen
          }
        }
      }

      //Every visible cell is at least a valid 1x1 rectangle, so best
      //always exists here.
      for(rp = best.r0; rp < best.r1; ++rp) {
        for(qp = best.q0; qp < best.q1; ++qp) {
          covered[rp * shapeU + qp] = 1
        }
      }

      rects.push({
        q0: best.q0, r0: best.r0, q1: best.q1, r1: best.r1,
        diag: best.diag,
        corners: {
          a00: grid.get(best.q0, best.r0).c00,
          a10: grid.get(best.q1 - 1, best.r0).c10,
          a11: grid.get(best.q1 - 1, best.r1 - 1).c11,
          a01: grid.get(best.q0, best.r1 - 1).c01
        },
        tex: seed.tex,
        facingFlip: seed.flip
      })
    }
  }

  return rects
}

//
// Exact mesh computation
//

function computeExactMesh(array, voxelSideTextureIDs) {
  var shp = array.shape.slice(0)
  var nx = (shp[0] - 2) | 0
  var ny = (shp[1] - 2) | 0
  var nz = (shp[2] - 2) | 0
  var sz = nx * ny * nz
  var scratch0 = pool.mallocInt32(sz)
  var scratch1 = pool.mallocInt32(sz)
  var scratch2 = pool.mallocInt32(sz)
  var rshp = [nx, ny, nz]
  var ao0 = ndarray(scratch0, rshp)
  var ao1 = ndarray(scratch1, rshp)
  var ao2 = ndarray(scratch2, rshp)

  //Same AO field calculation as the legacy path.
  mesh.surfaceStencil(ao0, ao1, ao2, array)

  var builder = new MeshBuilder()
  builder.voxelSideTextureIDs = voxelSideTextureIDs

  var buffers = [ao0, ao1, ao2]
  for(var d = 0; d < 3; ++d) {
    var u = (d + 1) % 3
    var v = (d + 2) % 3

    var st = buffers[d].transpose(d, u, v)
    var shapeU = rshp[u]
    var shapeV = rshp[v]
    var n = rshp[d] | 0

    builder.d = d
    builder.u = v
    builder.v = u

    for(var i = 0; i < n; ++i) {
      var slice = st.pick(i)
      var grid = buildGrid(slice, shapeU, shapeV, d, voxelSideTextureIDs)
      var rects = planSlice(grid)
      for(var ri = 0; ri < rects.length; ++ri) {
        var rect = rects[ri]
        builder.z = i
        emitRectangle(builder, rect.q0, rect.r0, rect.q1, rect.r1,
                      rect.facingFlip, rect.diag, rect.corners, rect.tex)
      }
    }
  }

  pool.freeInt32(scratch0)
  pool.freeInt32(scratch1)
  pool.freeInt32(scratch2)

  if(builder.ptr === 0) {
    pool.freeUint8(builder.buffer)
    return null
  }

  var out = builder.buffer.subarray(0, builder.ptr)
  //Copy out of the pooled buffer so callers own the result, matching the
  //legacy contract.
  var result = new Uint8Array(out.length)
  result.set(out)
  pool.freeUint8(builder.buffer)
  return result
}

//Enforce the byte-expressible coordinate range of the legacy vertex format.
function exactAO(array, voxelSideTextureIDs) {
  var shp = array.shape
  for(var i = 0; i < 3; ++i) {
    //Interior core voxels span shp-2 cells; the largest emitted coordinate
    //is that core extent, which must fit in one unsigned byte.
    if((shp[i] - 2) > MAX_COORD) {
      throw new RangeError("exactAO: coordinate range exceeds byte vertex format (axis " + i + ")")
    }
  }
  return computeExactMesh(array, voxelSideTextureIDs)
}

module.exports = exactAO
module.exports.rectCompatible = rectCompatible
module.exports.faceFlipAO = faceFlipAO
module.exports.buildGrid = buildGrid
module.exports.planSlice = planSlice
module.exports.EPS = EPS
