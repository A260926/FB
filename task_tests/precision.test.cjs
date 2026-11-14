const assert = require('node:assert/strict')
const test = require('node:test')

const jscad = require('../packages/modeling/src')
const { booleans, geometries, measurements, maths, primitives, transforms } = jscad

const THICKNESS = 2 ** -20
const FAR = 2 ** 20

const polygonsOf = (geometry) => geometries.geom3.toPolygons(geometry)
const volumeOf = (geometry) => measurements.measureVolume(geometry)

const relativeError = (actual, expected) => Math.abs(actual / expected - 1)
const assertRelative = (actual, expected, tolerance, message) => {
  const error = relativeError(actual, expected)
  assert.ok(error <= tolerance, `${message}: actual=${actual} expected=${expected} relative_error=${error}`)
}

// Signed volume measured in local coordinates. A model placed far from the
// origin is re-centred first, so the measurement itself is not swamped by the
// coordinate magnitude.
const localVolumeOf = (geometry) => {
  const bounds = measurements.measureBoundingBox(geometry)
  const centre = [0, 1, 2].map((axis) => (bounds[0][axis] + bounds[1][axis]) / 2)
  const matrix = maths.mat4.fromTranslation(maths.mat4.create(), centre.map((value) => -value))
  return volumeOf(transforms.transform(matrix, geometry))
}

// Determinant of the linear part of a mat4, i.e. the volume scale factor.
const determinantOf = (matrix) => {
  const a = matrix[0]
  const b = matrix[4]
  const c = matrix[8]
  const d = matrix[1]
  const e = matrix[5]
  const f = matrix[9]
  const g = matrix[2]
  const h = matrix[6]
  const i = matrix[10]
  return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
}

// --------------------------------------------------------------------------
// Watertightness / orientation probes.
//
// The oriented boundary is validated by matching every directed edge against
// the oppositely directed edge(s) of a neighbouring face. A long edge may be
// matched by several collinear short edges, so a directed edge is accepted
// when the opposite-direction sub-segments cover it completely. Shared
// internal faces are cancelled by the boolean and therefore leave no unmatched
// edges.
// --------------------------------------------------------------------------

const subtract = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
const lengthOf = (a) => Math.sqrt(dot(a, a))

const collectEdges = (geometry) => {
  const edges = []
  for (const polygon of polygonsOf(geometry)) {
    const vertices = polygon.vertices
    for (let i = 0; i < vertices.length; i++) {
      edges.push({ start: vertices[i], end: vertices[(i + 1) % vertices.length] })
    }
  }
  return edges
}

const surfaceProblems = (geometry) => {
  const polygons = polygonsOf(geometry)
  const problems = []
  const scale = polygons.reduce((largest, polygon) => {
    for (const vertex of polygon.vertices) {
      const vertexScale = Math.max(Math.abs(vertex[0]), Math.abs(vertex[1]), Math.abs(vertex[2]))
      if (vertexScale > largest) largest = vertexScale
    }
    return largest
  }, 1)
  const tolerance = 2 ** 10 * Number.EPSILON * scale
  const edges = collectEdges(geometry)

  const near = (a, b) => lengthOf(subtract(a, b)) <= tolerance

  // Split every directed edge at the endpoints of any other edge that lie on
  // it. This normalises T-junctions where one long edge is met by several
  // collinear short edges, so afterwards each elementary sub-segment must have
  // exactly one oppositely directed counterpart.
  const splitPoints = (edge, others) => {
    const direction = subtract(edge.end, edge.start)
    const segmentLength = lengthOf(direction)
    if (segmentLength <= tolerance) return []
    const unit = [direction[0] / segmentLength, direction[1] / segmentLength, direction[2] / segmentLength]
    const points = [0, segmentLength]
    for (const other of others) {
      for (const point of [other.start, other.end]) {
        const offset = subtract(point, edge.start)
        const perpendicular = lengthOf(cross(direction, offset)) / segmentLength
        if (perpendicular > tolerance) continue
        const projection = dot(offset, unit)
        if (projection > tolerance && projection < segmentLength - tolerance) points.push(projection)
      }
    }
    points.sort((a, b) => a - b)
    const unique = []
    for (const point of points) {
      if (unique.length === 0 || point - unique[unique.length - 1] > tolerance) unique.push(point)
    }
    return unique.map((projection) => [
      edge.start[0] + unit[0] * projection,
      edge.start[1] + unit[1] * projection,
      edge.start[2] + unit[2] * projection
    ])
  }

  // Build the list of elementary directed sub-segments.
  const pieces = []
  for (let i = 0; i < edges.length; i++) {
    const others = edges.filter((edge, k) => k !== i)
    const points = splitPoints(edges[i], others)
    if (points.length < 2) continue
    for (let j = 0; j + 1 < points.length; j++) pieces.push({ start: points[j], end: points[j + 1], index: i })
  }

  // Every elementary piece must be matched by an opposite directed piece.
  const used = new Array(pieces.length).fill(false)
  for (let i = 0; i < pieces.length; i++) {
    if (used[i]) continue
    const piece = pieces[i]
    let matched = false
    for (let k = 0; k < pieces.length; k++) {
      if (k === i || used[k]) continue
      const other = pieces[k]
      if (near(piece.end, other.start) && near(piece.start, other.end)) {
        used[i] = used[k] = true
        matched = true
        break
      }
    }
    if (!matched) problems.push(`unmatched directed edge ${JSON.stringify(piece.start)} -> ${JSON.stringify(piece.end)}`)
  }
  return problems
}

const assertWatertight = (geometry, name) => {
  const problems = surfaceProblems(geometry)
  assert.equal(problems.length, 0, `${name}: surface is not watertight\n${problems.slice(0, 10).join('\n')}`)
}

// Smallest distance from a point to any edge of the given geometry.
const distanceToBoundary = (geometry, point) => {
  let smallest = Infinity
  for (const edge of collectEdges(geometry)) {
    const direction = subtract(edge.end, edge.start)
    const segmentLengthSquared = dot(direction, direction)
    let t = segmentLengthSquared === 0 ? 0 : dot(subtract(point, edge.start), direction) / segmentLengthSquared
    t = Math.max(0, Math.min(1, t))
    const closest = [edge.start[0] + t * direction[0], edge.start[1] + t * direction[1], edge.start[2] + t * direction[2]]
    smallest = Math.min(smallest, lengthOf(subtract(point, closest)))
  }
  return smallest
}

// A probe strictly inside the solid, well away from any boundary.
const assertInteriorProbe = (geometry, point, name) => {
  const bounds = measurements.measureBoundingBox(geometry)
  for (let axis = 0; axis < 3; axis++) {
    assert.ok(point[axis] >= bounds[0][axis] && point[axis] <= bounds[1][axis], `${name}: probe ${point} lies outside the bounds`)
  }
  assert.ok(distanceToBoundary(geometry, point) > 0, `${name}: probe ${point} should be strictly inside`)
}

// The local thickness of a thin slab: the volume of its intersection with a
// thin cuboid probe centred inside the slab and spanning its faces. The caller
// supplies the expected thickness so the measured thickness can be checked
// directly, rather than inferred from a large total volume.
const probeVolume = (geometry, centre, size) => {
  const probe = transforms.translate(centre, primitives.cuboid({ size }))
  return volumeOf(booleans.intersect(geometry, probe))
}

// Axis-aligned bounding box of the given geometry.
const boundsOf = (geometry) => measurements.measureBoundingBox(geometry)

// A matrix combining rotation, shear, non-uniform scaling (and optionally a
// mirror), then translation.
const buildAffine = ({ angles, shear, scale, mirror, translation }) => {
  const rotation = maths.mat4.fromTaitBryanRotation(maths.mat4.create(), angles[2], angles[1], angles[0])
  const shearMatrix = maths.mat4.fromValues(
    shear[0], shear[1], shear[2], 0,
    shear[3], shear[4], shear[5], 0,
    shear[6], shear[7], shear[8], 0,
    0, 0, 0, 1
  )
  const scaleMatrix = maths.mat4.fromScaling(maths.mat4.create(), scale)
  let linear = maths.mat4.multiply(maths.mat4.create(), rotation, shearMatrix)
  linear = maths.mat4.multiply(maths.mat4.create(), linear, scaleMatrix)
  if (mirror) {
    const mirrorMatrix = maths.mat4.fromValues(
      -1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 1, 0,
      0, 0, 0, 1
    )
    linear = maths.mat4.multiply(maths.mat4.create(), linear, mirrorMatrix)
  }
  const translationMatrix = maths.mat4.fromTranslation(maths.mat4.create(), translation)
  const matrix = maths.mat4.multiply(maths.mat4.create(), translationMatrix, linear)
  return { matrix, volumeScale: Math.abs(determinantOf(linear)) }
}

// --------------------------------------------------------------------------
// thin-slabs
// --------------------------------------------------------------------------

test('thin-slabs', async (t) => {
  // The reported case: intersect a 2x2x2 cuboid with a [3,3,2^-20] cuboid.
  // The result must be a closed six-faced thin box of volume 4 * 2^-20.
  await t.test('flat thin intersection produces a closed six-sided box', () => {
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [3, 3, THICKNESS] })
    const result = booleans.intersect(cube, slab)

    const expected = 4 * THICKNESS
    assertRelative(volumeOf(result), expected, 1e-3, 'thin intersection volume')
    assert.equal(polygonsOf(result).length, 6, 'thin intersection should be a six-faced box')
    assertWatertight(result, 'thin intersection')

    // Sign: the oriented surface encloses positive volume.
    assert.ok(volumeOf(result) > 0, 'thin intersection must have positive oriented volume')

    // The slab's own thickness is what remains; the box spans x and y fully.
    const bounds = boundsOf(result)
    assertRelative(bounds[0][2], -THICKNESS / 2, 1e-3, 'thin intersection lower z face')
    assertRelative(bounds[1][2], THICKNESS / 2, 1e-3, 'thin intersection upper z face')
    assertRelative(bounds[1][0] - bounds[0][0], 2, 1e-3, 'thin intersection x extent')
    assertRelative(bounds[1][1] - bounds[0][1], 2, 1e-3, 'thin intersection y extent')
  })

  await t.test('thin subtraction leaves two halves with a complete surface on each side', () => {
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [3, 3, THICKNESS] })
    const result = booleans.subtract(cube, slab)

    const expected = 8 - 4 * THICKNESS
    assertRelative(volumeOf(result), expected, 1e-3, 'thin subtraction volume')

    // Both cut faces must survive: one on each side of the gap. Probe them with
    // thin brackets just above and just below the slab, and check each cut
    // face is intact over the whole 2x2 footprint.
    const belowCentre = [0, 0, -(THICKNESS / 2 + THICKNESS)]
    const aboveCentre = [0, 0, THICKNESS / 2 + THICKNESS]
    const bracket = 2 * THICKNESS
    assertRelative(probeVolume(result, belowCentre, [2, 2, bracket]), 4 * bracket, 1e-3, 'lower cut face')
    assertRelative(probeVolume(result, aboveCentre, [2, 2, bracket]), 4 * bracket, 1e-3, 'upper cut face')

    // A probe through the middle of the removed slab sees nothing.
    assert.equal(probeVolume(result, [0, 0, 0], [2, 2, THICKNESS / 2]), 0, 'removed slab interior must be empty')

    assertWatertight(result, 'thin subtraction')
    assert.ok(distanceToBoundary(result, [0, 0, -(THICKNESS / 2 + THICKNESS)]) > 0, 'lower half interior probe')
    assert.ok(distanceToBoundary(result, [0, 0, THICKNESS / 2 + THICKNESS]) > 0, 'upper half interior probe')
  })

  await t.test('diagonal thin cuts keep both cut surfaces', () => {
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const diagonal = transforms.rotate([Math.PI / 4, 0, 0], primitives.cuboid({ size: [100, 100, THICKNESS] }))
    const result = booleans.subtract(cube, diagonal)

    // The intersection of a 2x2x2 cube with a 45-degree plane is a 2 x 2*sqrt(2) rectangle,
    // so the cut removes 2 * 2*sqrt(2) * t.
    const cutArea = 2 * (2 * Math.sqrt(2))
    assertRelative(volumeOf(result), 8 - cutArea * THICKNESS, 1e-3, 'diagonal cut volume')
    assertWatertight(result, 'diagonal cut')

    // Probe each half along the cut normal, away from the boundary.
    for (const sign of [-1, 1]) {
      const offset = (THICKNESS / 2 + THICKNESS) * sign
      const centre = [0, (offset * Math.sqrt(2)) / 2, (offset * Math.sqrt(2)) / 2]
      const halfWidth = THICKNESS
      const probe = booleans.intersect(result, transforms.translate(centre, primitives.cuboid({ size: [1, 1, THICKNESS * 4] })))
      // A probe spanning the gap on either side must have exactly the thickness
      // of one half-depth, not the whole 2 units.
      assert.ok(volumeOf(probe) < 1, 'diagonal probe must not span both halves')
    }
  })

  await t.test('thin slabs separated by 2^-20 stay distinct', () => {
    // Two layers 2^-20 apart must not merge: subtract both from a large cube,
    // leaving the 2^-20 wall between them.
    const s1 = transforms.translate([0, 0, THICKNESS], primitives.cuboid({ size: [4, 4, THICKNESS] }))
    const s2 = transforms.translate([0, 0, -THICKNESS], primitives.cuboid({ size: [4, 4, THICKNESS] }))
    const cube = primitives.cuboid({ size: [6, 6, 6] })
    const result = booleans.subtract(cube, [s1, s2])

    assertRelative(volumeOf(result), 6 * 6 * 6 - 2 * 4 * 4 * THICKNESS, 1e-3, 'two layer subtraction')
    assertWatertight(result, 'two layer subtraction')

    // The surviving 2^-20 wall in the middle: intersect with the middle
    // region so only the wall remains.
    const middle = transforms.translate([0, 0, 0], primitives.cuboid({ size: [4, 4, THICKNESS] }))
    const wall = booleans.intersect(result, middle)
    assertRelative(volumeOf(wall), 4 * 4 * THICKNESS, 1e-3, 'surviving middle wall volume')
    assertWatertight(wall, 'surviving middle wall')
    assert.ok(distanceToBoundary(wall, [0, 0, 0]) > 0, 'middle wall interior probe')
  })

  await t.test('repeated boolean operations do not resynthesise or duplicate surfaces', () => {
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [100, 100, THICKNESS] })

    const cut = booleans.subtract(cube, slab)
    const removed = booleans.subtract(cube, cut)
    const restored = booleans.union(cut, removed)
    const cutAgain = booleans.subtract(restored, cut)

    assertRelative(volumeOf(cut), 8 - 4 * THICKNESS, 1e-3, 'first cut')
    assertRelative(volumeOf(removed), 4 * THICKNESS, 1e-3, 'removed slab portion')
    assertRelative(volumeOf(restored), 8, 1e-3, 'restored cube')
    assertRelative(volumeOf(cutAgain), 4 * THICKNESS, 1e-3, 'cut again')

    // The restored cube must be a single closed layer: re-running union with
    // itself or intersect with itself must not add internal faces.
    const selfUnion = booleans.union(restored, restored)
    assertRelative(volumeOf(selfUnion), 8, 1e-3, 'idempotent union')
    assertWatertight(selfUnion, 'idempotent union')
    const selfIntersect = booleans.intersect(restored, restored)
    assertRelative(volumeOf(selfIntersect), 8, 1e-3, 'idempotent intersect')
    assertWatertight(selfIntersect, 'idempotent intersect')

    // The thin piece must survive a round trip through intersect as well.
    assertRelative(volumeOf(booleans.intersect(cut, cut)), 8 - 4 * THICKNESS, 1e-3, 'idempotent thin intersect')
  })

  await t.test('shared faces are cancelled, not left as internal surfaces', () => {
    // Two unit cubes sharing a face union into one box; the shared face must be
    // gone and the result must be a closed six-faced box.
    const left = primitives.cuboid({ size: [1, 1, 1], center: [-0.5, 0, 0] })
    const right = primitives.cuboid({ size: [1, 1, 1], center: [0.5, 0, 0] })
    const result = booleans.union(left, right)

    assertRelative(volumeOf(result), 2, 1e-3, 'shared face union volume')
    assert.equal(polygonsOf(result).length, 6, 'shared face union should be one closed box')
    assertWatertight(result, 'shared face union')

    // Subtract one from the other: the shared face must be a real boundary on
    // the remaining half.
    const difference = booleans.subtract(left, right)
    assertRelative(volumeOf(difference), 1, 1e-3, 'shared face subtraction volume')
    assertWatertight(difference, 'shared face subtraction')
  })

  await t.test('a long edge may pair with several collinear short edges', () => {
    // A cuboid whose face is split by touching geometry: union with a smaller
    // cuboid that only covers part of a face produces T-junctions, where a long
    // edge meets two collinear short edges. The surface must still close.
    const large = primitives.cuboid({ size: [4, 4, 4] })
    // Spans x in [0.5, 2.5]: half overlaps the large solid, half protrudes, so
    // the shared part of the +x face is inset from the edges (T-junctions).
    const small = primitives.cuboid({ size: [2, 2, 2], center: [1.5, 0, 0] })
    const result = booleans.union(large, small)

    // 4x4x4 plus the 2x2x2 block, minus the 2x2x1.5 overlap.
    assertRelative(volumeOf(result), 64 + 8 - 2 * 2 * 1.5, 1e-3, 't-junction union volume')
    assertWatertight(result, 't-junction union')

    const difference = booleans.subtract(large, small)
    assertRelative(volumeOf(difference), 64 - 2 * 2 * 1.5, 1e-3, 't-junction subtraction volume')
    assertWatertight(difference, 't-junction subtraction')
  })
})

// --------------------------------------------------------------------------
// mixed-scale
// --------------------------------------------------------------------------

test('mixed-scale', async (t) => {
  await t.test('a tiny solid next to a large one is not swallowed', () => {
    const large = primitives.cuboid({ size: [2, 2, 2] })
    const tiny = transforms.translate([1 + THICKNESS / 2, 0, 0], primitives.cuboid({ size: [THICKNESS, THICKNESS, THICKNESS] }))

    const union = booleans.union(large, tiny)
    assertRelative(volumeOf(union), 8 + THICKNESS ** 3, 1e-9, 'union keeps tiny solid')
    // The tiny block sits outside the large one, attached to its +x face, so the
    // union must have more than six faces; it must not have collapsed to the
    // large solid.
    assert.ok(polygonsOf(union).length > 6, 'union must retain the tiny solid')
    assertWatertight(union, 'mixed scale union')

    // Subtracting the large solid from the union leaves exactly the tiny block.
    const remainder = booleans.subtract(union, large)
    assertRelative(volumeOf(remainder), THICKNESS ** 3, 1e-9, 'remainder is the tiny solid')
    assertWatertight(remainder, 'mixed scale remainder')

    // And the tiny block is not lost when subtracted from the union itself.
    const withoutTiny = booleans.subtract(union, tiny)
    assertRelative(volumeOf(withoutTiny), 8, 1e-9, 'union without tiny')
  })

  await t.test('a tiny solid inside a large one is resolved as a cavity', () => {
    const large = primitives.cuboid({ size: [4, 4, 4] })
    const tiny = primitives.cuboid({ size: [THICKNESS, THICKNESS, THICKNESS] })
    const result = booleans.subtract(large, tiny)

    assertRelative(volumeOf(result), 64 - THICKNESS ** 3, 1e-9, 'cavity volume')
    assertWatertight(result, 'tiny cavity')

    // The cavity walls exist: a probe inside the tiny region sees no material.
    // (Probe smaller than the cavity, exactly centred.)
    const probe = transforms.scale([0.5, 0.5, 0.5], primitives.cuboid({ size: [THICKNESS, THICKNESS, THICKNESS] }))
    assert.equal(volumeOf(booleans.intersect(result, probe)), 0, 'cavity must be empty')

    // Re-union with the tiny solid restores the large solid.
    const restored = booleans.union(result, tiny)
    assertRelative(volumeOf(restored), 64, 1e-9, 'restored large solid')
  })

  await t.test('thin slabs far from the origin keep their local thickness', () => {
    // A thin slab next to a large solid, both translated far away. The
    // coordinates are so large that the thickness only survives because
    // tolerances follow the local coordinate magnitude.
    const offset = [FAR, -FAR, FAR]
    const large = transforms.translate(offset, primitives.cuboid({ size: [2, 2, 2] }))
    const slab = transforms.translate(offset, primitives.cuboid({ size: [100, 100, THICKNESS] }))
    const result = booleans.intersect(large, slab)

    // 4 * 2^-20, measured in local coordinates.
    const expected = 4 * THICKNESS
    assertRelative(localVolumeOf(result), expected, 1e-3, 'far thin intersection local volume')
    assertWatertight(result, 'far thin intersection')

    const bounds = boundsOf(result)
    assertRelative(bounds[1][2] - bounds[0][2], THICKNESS, 1e-3, 'far thin slab thickness')
  })

  await t.test('mixed sizes in one connected component', () => {
    // A large block, a small block, and a thin bridge connecting them: one
    // connected component with features spanning many orders of magnitude.
    const large = primitives.cuboid({ size: [2, 2, 2], center: [-1 - THICKNESS / 2, 0, 0] })
    const small = primitives.cuboid({ size: [THICKNESS, THICKNESS, THICKNESS], center: [1 + THICKNESS / 2, 0, 0] })
    const bridge = primitives.cuboid({ size: [2, THICKNESS, THICKNESS], center: [0, 0, 0] })
    const result = booleans.union(large, bridge, small)

    const expected = 8 + 2 * THICKNESS ** 2 + THICKNESS ** 3
    assertRelative(volumeOf(result), expected, 1e-3, 'mixed connected component volume')
    assertWatertight(result, 'mixed connected component')

    // The component is connected: intersecting the probe at the middle of the
    // bridge finds material, and at the ends finds the two blocks.
    assertRelative(probeVolume(result, [0, 0, 0], [THICKNESS / 2, THICKNESS / 2, THICKNESS / 2]), (THICKNESS / 2) ** 3, 1e-3, 'bridge probe')
    assertRelative(probeVolume(result, [-(1 + THICKNESS / 2), 0, 0], [1, 1, 1]), 1, 1e-3, 'large block probe')
    assertRelative(probeVolume(result, [1 + THICKNESS / 2, 0, 0], [THICKNESS, THICKNESS, THICKNESS]), THICKNESS ** 3, 1e-3, 'small block probe')
  })

  await t.test('a 2^20 solid and a 2^-20 feature in the same operation', () => {
    // Large solid with a tiny notch removed near its centre.
    const large = primitives.cuboid({ size: [4, 4, 4], center: [FAR, 0, 0] })
    const notch = primitives.cuboid({ size: [THICKNESS, 1, 1], center: [FAR, 0, 0] })
    const result = booleans.subtract(large, notch)

    assertRelative(localVolumeOf(result), 64 - THICKNESS, 1e-3, 'large solid with tiny notch')
    assertWatertight(result, 'large solid with tiny notch')
  })
})

// --------------------------------------------------------------------------
// affine-reuse
// --------------------------------------------------------------------------

test('affine-reuse', async (t) => {
  await t.test('operands transformed by one affine map keep their thin feature', () => {
    const affine = buildAffine({
      angles: [0.4, 0.2, 0.1],
      shear: [1, 0.2, 0, 0, 1, 0.1, 0, 0, 1],
      scale: [1.3, 0.8, 1.1],
      translation: [0, 0, 0]
    })
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [100, 100, THICKNESS] })

    const transformedCube = transforms.transform(affine.matrix, cube)
    const transformedSlab = transforms.transform(affine.matrix, slab)
    const result = booleans.intersect(transformedCube, transformedSlab)

    const expected = affine.volumeScale * 4 * THICKNESS
    assertRelative(volumeOf(result), expected, 1e-3, 'affine thin intersection volume')
    assertWatertight(result, 'affine thin intersection')

    // The local thickness is preserved: transform a probe and compare.
    const localProbe = transforms.scale([1, 1, 4], primitives.cuboid({ size: [1, 1, THICKNESS] }))
    const expectedProbe = affine.volumeScale * THICKNESS
    assertRelative(volumeOf(booleans.intersect(result, transforms.transform(affine.matrix, localProbe))), expectedProbe, 1e-2, 'affine probe thickness')
  })

  await t.test('mirrored affine operands keep orientation and volume', () => {
    const affine = buildAffine({
      angles: [0.3, 0.1, 0.2],
      shear: [1, 0, 0.1, 0, 1, 0, 0, 0, 1],
      scale: [1.2, 0.9, 1.0],
      mirror: true,
      translation: [0, 0, 0]
    })
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [100, 100, THICKNESS] })

    const result = booleans.intersect(transforms.transform(affine.matrix, cube), transforms.transform(affine.matrix, slab))
    const expected = affine.volumeScale * 4 * THICKNESS
    assertRelative(volumeOf(result), expected, 1e-3, 'mirrored affine thin intersection')
    assertWatertight(result, 'mirrored affine thin intersection')
  })

  await t.test('affine operands translated to coordinate magnitude 2^20', () => {
    const affine = buildAffine({
      angles: [0.4, 0.2, 0.1],
      shear: [1, 0.2, 0, 0, 1, 0.1, 0, 0, 1],
      scale: [1.3, 0.8, 1.1],
      translation: [FAR, -FAR, FAR]
    })
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [100, 100, THICKNESS] })

    const result = booleans.intersect(transforms.transform(affine.matrix, cube), transforms.transform(affine.matrix, slab))
    const expected = affine.volumeScale * 4 * THICKNESS
    assertRelative(localVolumeOf(result), expected, 1e-3, 'far affine thin intersection local volume')
    assertWatertight(result, 'far affine thin intersection')

    // Local thickness probe: translate a probe to the same place.
    const localProbe = transforms.transform(
      affine.matrix,
      transforms.scale([1, 1, 4], primitives.cuboid({ size: [1, 1, THICKNESS] }))
    )
    const probe = booleans.intersect(result, localProbe)
    assertRelative(localVolumeOf(probe), affine.volumeScale * THICKNESS, 1e-2, 'far affine local thickness')
  })

  await t.test('subtraction under an affine map keeps both cut sides', () => {
    const affine = buildAffine({
      angles: [0.25, 0.15, 0.05],
      shear: [1, 0.1, 0, 0, 1, 0, 0, 0, 1],
      scale: [1.1, 0.9, 1.2],
      translation: [FAR, 0, 0]
    })
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [100, 100, THICKNESS] })
    const result = booleans.subtract(transforms.transform(affine.matrix, cube), transforms.transform(affine.matrix, slab))

    const expected = affine.volumeScale * (8 - 4 * THICKNESS)
    assertRelative(localVolumeOf(result), expected, 1e-3, 'far affine subtraction local volume')
    assertWatertight(result, 'far affine subtraction')
  })

  await t.test('materialised and lazy operands can be reused', () => {
    // Reuse the same operands: one materialised geometry, one lazy (unapplied)
    // transform, one already used as an operand, all in several operations.
    const materialised = primitives.cuboid({ size: [2, 2, 2] })
    polygonsOf(materialised) // force materialisation

    const lazy = transforms.translate([1, 0, 0], transforms.rotate([0.2, 0.3, 0.4], primitives.cuboid({ size: [2, 2, 2] })))
    const consumed = primitives.cuboid({ size: [3, 3, THICKNESS] })

    const first = booleans.intersect(materialised, consumed)
    const second = booleans.intersect(materialised, lazy)
    const third = booleans.subtract(materialised, consumed)

    // The already-consumed operand is still usable and unchanged.
    const fourth = booleans.intersect(materialised, consumed)

    // Materialised geometry must be unchanged by its use as an operand.
    assertRelative(volumeOf(materialised), 8, 1e-9, 'materialised operand unchanged')
    assertRelative(volumeOf(first), 4 * THICKNESS, 1e-9, 'reuse: first intersection')
    assertRelative(volumeOf(fourth), 4 * THICKNESS, 1e-9, 'reuse: repeated intersection')
    assertRelative(volumeOf(third), 8 - 4 * THICKNESS, 1e-9, 'reuse: subtraction')

    // The lazy operand still evaluates to the same solid whether used before
    // or after another operation.
    const lazyVolume = volumeOf(booleans.intersect(materialised, lazy))
    assertRelative(lazyVolume, volumeOf(second), 1e-9, 'reuse: lazy operand consistent')
    assertWatertight(first, 'reuse: first intersection')
    assertWatertight(second, 'reuse: lazy intersection')
    assertWatertight(third, 'reuse: subtraction')
  })

  await t.test('the same operands are reusable across repeated operations', () => {
    const cube = primitives.cuboid({ size: [2, 2, 2] })
    const slab = primitives.cuboid({ size: [100, 100, THICKNESS] })
    const diagonal = transforms.rotate([Math.PI / 4, 0, 0], slab)

    const volumes = []
    const results = []
    for (let i = 0; i < 2; i++) {
      results.push(booleans.intersect(cube, slab))
      results.push(booleans.subtract(cube, slab))
      results.push(booleans.intersect(cube, diagonal))
    }
    for (let i = 0; i < 3; i++) volumes.push(volumeOf(results[i]))

    // Repeated applications must be identical and must not accumulate extra
    // faces.
    assertRelative(volumes[0], 4 * THICKNESS, 1e-3, 'repeat: intersection volume')
    assertRelative(volumes[1], 8 - 4 * THICKNESS, 1e-3, 'repeat: subtraction volume')
    assert.equal(volumeOf(results[3]), volumes[0], 'repeat: deterministic intersection')
    assert.equal(volumeOf(results[4]), volumes[1], 'repeat: deterministic subtraction')
    assertRelative(volumeOf(results[5]), volumes[2], 1e-9, 'repeat: deterministic diagonal')
    assertWatertight(results[5], 'repeat: diagonal intersection')
  })
})
