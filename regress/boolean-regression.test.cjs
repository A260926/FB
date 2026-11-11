'use strict'

/**
 * Order-independent spatial regression for the geom3 boolean pipeline
 * (lazy transform -> BSP booleans -> retessellate).
 *
 * Run with:   node --test regress/boolean-regression.test.cjs
 * or:         bash regress/check_booleans.sh
 *
 * Reference method (see boolean_reference.cjs for the implementation):
 * - every model is a finite boolean expression over axis-aligned cuboids;
 * - the six boundary planes of every cuboid cut space into open grid cells,
 *   and the reference volume is the exact sum of the cells whose centers
 *   satisfy the expression;
 * - reference inside/outside answers are the same expression evaluated at
 *   probe points that are guaranteed to stay off every boundary plane;
 * - output polygons are checked independently: fan triangulation plus a
 *   Moller-Trumbore ray cast, with same-distance intersections merged so a
 *   quad's shared diagonal edge is never counted twice;
 * - winding is checked by accumulating the signed tetrahedron volume of the
 *   triangulated output, which exposes inward-facing cavity walls even when
 *   the bounding box still looks correct.
 *
 * The expected values are never produced with measureVolume, BSP,
 * retessellate or another CSG library; the library's own measureVolume is
 * only used as the quantity under test.
 *
 * Excluded on purpose:
 * - probes / grid cells on boundary planes and at triangle vertices (the
 *   inside/outside convention on a boundary is undefined for this reference;
 *   boundary sets have zero volume anyway);
 * - curved or rotated primitives, and non-affine / singular transforms;
 * - triangle-count comparisons between the two transform pipelines (only
 *   occupied space, probe verdicts and signed volume are compared).
 */

const test = require('node:test')
const assert = require('node:assert/strict')

const modeling = require('../packages/modeling/src')
const ref = require('./boolean_reference.cjs')

const { OPS, box, expr, referenceVolume, referenceContains, mat4Determinant,
  transformPoint, pointInPolygons, signedVolumeOfPolygons } = ref

const { primitives, booleans, transforms, measurements, geometries, maths } = modeling
const { cuboid } = primitives
const { geom3 } = geometries
const { mat4, plane } = maths

const VOL_TOL = (expected) => 1e-7 * Math.max(1, Math.abs(expected))
const closeEnough = (actual, expected) => Math.abs(actual - expected) <= VOL_TOL(expected)

// Generic, deliberately irrational-looking ray directions so a ray is
// essentially never aligned with a triangle edge or aimed at a vertex.
const RAY_DIRECTIONS = [
  [0.371137, 0.871231, 1.0],
  [-0.51231, 0.77123, -0.63],
  [0.23, -0.91, 0.47]
]

/*
 * One shared, non-singular non-commuting affine combination for every case:
 *   world = translate . shear . mirror(X) . nonUniformScale
 * The factors do not commute (mirror flips the shear sign, the translation
 * moves the mirrored geometry), so swapping the application order changes
 * the occupied world space and the test would fail.
 */
const buildAffine = () => {
  const scale = mat4.fromScaling(mat4.create(), [1.7, 0.8, 2.3])
  const mirror = mat4.mirrorByPlane(
    mat4.create(), plane.fromNormalAndPoint(plane.create(), [1, 0, 0], [0, 0, 0])
  )
  // column-major: x' = x + 0.35y - 0.2z, y' = y, z' = z
  const shear = mat4.fromValues(
    1, 0, 0, 0,
    0.35, 1, 0, 0,
    -0.2, 0, 1, 0,
    0, 0, 0, 1
  )
  const translate = mat4.fromTranslation(mat4.create(), [3.5, -2.25, 1.125])
  let matrix = mat4.clone(translate)
  matrix = mat4.multiply(matrix, matrix, shear)
  matrix = mat4.multiply(matrix, matrix, mirror)
  matrix = mat4.multiply(matrix, matrix, scale)
  return matrix
}
const AFFINE = buildAffine()
const AFFINE_ABS_DET = Math.abs(mat4Determinant(AFFINE))
assert.ok(AFFINE_ABS_DET > 1e-9, 'fixture transform must be non-singular')
{
  // The name "non-commuting combination" is part of the contract, so prove it:
  // the shear factors do not commute with the mirror (nor with the scaling).
  const mirror = mat4.mirrorByPlane(
    mat4.create(), plane.fromNormalAndPoint(plane.create(), [1, 0, 0], [0, 0, 0])
  )
  const shear = mat4.fromValues(
    1, 0, 0, 0,
    0.35, 1, 0, 0,
    -0.2, 0, 1, 0,
    0, 0, 0, 1
  )
  const ab = mat4.multiply(mat4.create(), mirror, shear)
  const ba = mat4.multiply(mat4.create(), shear, mirror)
  assert.ok(ab.some((v, i) => Math.abs(v - ba[i]) > 1e-12),
    'fixture shear and mirror must not commute')
}
const TRANSFORM_LABEL = 'world = translate(3.5,-2.25,1.125) . shear(0.35xy,-0.2xz) . mirrorX . scale(1.7,0.8,2.3)'

/*
 * The case table. Each case pairs a reference boolean expression over boxes
 * with probes given in model space; probes never touch a boundary plane or a
 * box corner. Asymmetric dimensions make operand/association mistakes change
 * the occupied volume.
 */
const cases = []

// 1. Through hole: a tall tool punches all the way through the host along Y.
{
  const host = box([-5, -3.5, -2.5], [5, 3.5, 2.5])
  const tool = box([-1.5, -5.5, -1.25], [1.5, 5.5, 1.25])
  const root = expr(OPS.SUBTRACT, host, tool)
  cases.push({
    name: 'through-hole',
    label: 'subtract(cuboid 10x7x5, cuboid 3x11x2.5 through Y)',
    root,
    empty: false,
    probes: [
      { p: [-3.5, 0.75, 0.5], inside: true, note: 'solid shell beside the hole' },
      { p: [0.3, -0.4, 0.6], inside: false, note: 'inside the through hole' },
      { p: [6.2, 0, 0], inside: false, note: 'outside beside host' },
      { p: [0, 0, 7.5], inside: false, note: 'outside above host' }
    ]
  })
}

// 2. Sealed internal cavity: the tool is fully enclosed, so an inner wall set
//    must exist with inward-facing winding.
{
  const host = box([-5, -3.5, -2.5], [5, 3.5, 2.5])
  const cavity = box([-2, -1.5, -1.5], [2, 1.5, 1.5])
  const root = expr(OPS.SUBTRACT, host, cavity)
  cases.push({
    name: 'sealed-cavity',
    label: 'subtract(cuboid 10x7x5, enclosed cuboid 4x3x3)',
    root,
    empty: false,
    probes: [
      { p: [0.4, -0.3, 0.7], inside: false, note: 'inside the sealed cavity' },
      { p: [3.6, 0.2, -0.4], inside: true, note: 'shell material around cavity' },
      { p: [0, 0, 7.5], inside: false, note: 'outside above host' }
    ]
  })
}

// 3. Two disconnected solids in one union result.
{
  const a = box([-4, -3, -2], [0, 3, 2])
  const b = box([3, -2, -3], [7, 2, 3])
  const root = expr(OPS.UNION, a, b)
  cases.push({
    name: 'disconnected-pair',
    label: 'union(cuboid 4x6x4, separated cuboid 4x4x6)',
    root,
    empty: false,
    probes: [
      { p: [-2.1, 0.4, -0.6], inside: true, note: 'inside first disconnected solid' },
      { p: [5.3, -0.7, 1.2], inside: true, note: 'inside second disconnected solid' },
      { p: [1.5, 0, 0], inside: false, note: 'gap between the solids' },
      { p: [1.5, 0, 8.5], inside: false, note: 'outside above both' }
    ]
  })
}

// 4. Two boxes sharing one whole face (zero-volume contact).
{
  const a = box([-2, -3, -1.5], [2, 3, 1.5])
  const b = box([2, -3, -1.5], [5, 3, 1.5])
  const root = expr(OPS.UNION, a, b)
  cases.push({
    name: 'shared-face',
    label: 'union(cuboid 4x6x3, cuboid 3x6x3 sharing the x=2 face)',
    root,
    empty: false,
    probes: [
      { p: [-0.7, 0.6, -0.8], inside: true, note: 'inside first box' },
      { p: [3.6, -0.5, 0.9], inside: true, note: 'inside second box' },
      { p: [0, 0, 4.5], inside: false, note: 'outside above the pair' }
    ]
  })
}

// 5. Identical solid subtracted from itself: the result is empty.
{
  const a = box([-4.5, -3.5, -2.5], [4.5, 3.5, 2.5])
  const root = expr(OPS.SUBTRACT, a, box([-4.5, -3.5, -2.5], [4.5, 3.5, 2.5]))
  cases.push({
    name: 'identical-subtract-empty',
    label: 'subtract(cuboid 9x7x5, identical cuboid 9x7x5)',
    root,
    empty: true,
    probes: [
      { p: [0, 0, 7], inside: false, note: 'where the solid used to be' },
      { p: [6, 6, 6], inside: false, note: 'well outside' }
    ]
  })
}

// 6. Three-box parenthesized difference (A - B) - C with overlapping,
//    asymmetric tools. B and C overlap inside A, so a wrong association such
//    as A - (B - C) would wrongly keep the overlap region occupied.
{
  const a = box([-4.5, -3.5, -2.5], [4.5, 3.5, 2.5])
  const b = box([-2, -4.5, -1.5], [2, 4.5, 1.5])
  const c = box([-3.5, -2.5, -2.5], [0.5, 2.5, 2.5])
  const root = expr(OPS.SUBTRACT, expr(OPS.SUBTRACT, a, b), c)
  cases.push({
    name: 'three-box-parenthesized-difference',
    label: 'subtract(subtract(cuboid 9x7x5, off-center cuboid 4x9x3), offset cuboid 4x5x5)',
    root,
    empty: false,
    probes: [
      { p: [1.5, 0.3, -0.4], inside: false, note: 'removed by first tool only' },
      { p: [-3.0, 0.2, 0.8], inside: false, note: 'removed by second tool only' },
      { p: [-0.7, 0, 0.5], inside: false, note: 'B intersect C overlap, must be empty' },
      { p: [3.6, -2.9, 1.6], inside: true, note: 'surviving shell corner' },
      { p: [-3.9, 0, 0], inside: true, note: 'surviving shell beside second tool' },
      { p: [0, 0, 6.5], inside: false, note: 'outside above' }
    ]
  })
}

// 7. Plain intersection with a genuine overlap volume, so all three boolean
//    operators travel the BSP + retessellate path in this suite.
{
  const a = box([-4, -3, -2], [2, 3, 2])
  const b = box([0, -2, -3], [5, 2, 1])
  const root = expr(OPS.INTERSECT, a, b)
  cases.push({
    name: 'overlapping-intersect',
    label: 'intersect(cuboid 6x6x4, cuboid 5x4x4)',
    root,
    empty: false,
    probes: [
      { p: [0.9, 0.4, -0.7], inside: true, note: 'inside the overlap' },
      { p: [-2.5, 0, 0], inside: false, note: 'first box only' },
      { p: [3.2, 0, 0], inside: false, note: 'second box only' },
      { p: [0.7, 0.3, 6.8], inside: false, note: 'outside above' }
    ]
  })
}

const applyOp = (op, left, right) => {
  if (op === OPS.UNION) return booleans.union(left, right)
  if (op === OPS.INTERSECT) return booleans.intersect(left, right)
  return booleans.subtract(left, right)
}

const makeCuboid = (b, leafMatrix) => {
  const center = b.min.map((lo, i) => (lo + b.max[i]) / 2)
  const size = b.min.map((lo, i) => b.max[i] - lo)
  const geometry = cuboid({ center, size })
  return leafMatrix ? transforms.transform(leafMatrix, geometry) : geometry
}

// Compile the reference expression into library calls in the same tree order.
const compileExpression = (root, leafMatrix) => {
  const walk = (node) => {
    if (node.op === undefined) return makeCuboid(node, leafMatrix)
    return applyOp(node.op, walk(node.left), walk(node.right))
  }
  return walk(root)
}

// Same compilation, but reusing externally supplied leaf geometries so the
// lazy-matrix materialization tests can re-run reused operands.
const compileWithLeaves = (root, leaves) => {
  let index = 0
  const walk = (node) => {
    if (node.op === undefined) return leaves[index++]
    return applyOp(node.op, walk(node.left), walk(node.right))
  }
  const result = walk(root)
  assert.equal(index, leaves.length, 'leaf count must match expression')
  return result
}

const collectLeaves = (root, leafMatrix) => {
  const leaves = []
  const walk = (node) => {
    if (node.op === undefined) { leaves.push(makeCuboid(node, leafMatrix)); return }
    walk(node.left)
    walk(node.right)
  }
  walk(root)
  return leaves
}

const castProbe = (polygons, worldProbe, expected, context) => {
  for (const dir of RAY_DIRECTIONS) {
    const actual = pointInPolygons(polygons, worldProbe, dir)
    assert.equal(
      actual, expected,
      `${context}: ray ${dir.join('/')} probe (${worldProbe.map((v) => v.toFixed(6)).join(', ')}) ` +
      `classified ${actual}, expected ${expected}`
    )
  }
}

const describeFailure = (testCase, transformLabel, detail) =>
  `case "${testCase.name}" [${testCase.label}] with ${transformLabel}: ${detail}`

test('boolean-occupancy', async (t) => {
  for (const testCase of cases) {
    await t.test(testCase.name, () => {
      const expectedModelVolume = referenceVolume(testCase.root)
      for (const probe of testCase.probes) {
        assert.equal(
          referenceContains(testCase.root, probe.p), probe.inside,
          `probe table error in ${testCase.name} at ${probe.p.join(',')}`
        )
      }

      // Pipeline 1: boolean in model space, no transform.
      const modelResult = compileExpression(testCase.root, null)
      const modelPolygons = geom3.toPolygons(modelResult)
      const modelVolume = measurements.measureVolume(modelResult)
      assert.ok(
        closeEnough(modelVolume, expectedModelVolume),
        describeFailure(testCase, 'identity transform',
          `model volume ${modelVolume} differs from reference ${expectedModelVolume}`)
      )
      if (testCase.empty) {
        assert.equal(modelPolygons.length, 0,
          describeFailure(testCase, 'identity transform', 'empty result must carry no polygons'))
      } else {
        assert.ok(modelPolygons.length > 0,
          describeFailure(testCase, 'identity transform', 'non-empty result has no polygons'))
      }
      for (const probe of testCase.probes) {
        if (testCase.empty) assert.equal(probe.inside, false, 'empty cases may only use external probes')
        castProbe(modelPolygons, probe.p, probe.inside,
          describeFailure(testCase, 'identity transform', probe.note))
      }

      // Pipeline 2a: transform the operands first (lazy matrices), then boolean.
      const operandFirst = compileExpression(testCase.root, AFFINE)
      // Pipeline 2b: boolean first, then transform the whole result.
      const resultFirst = transforms.transform(AFFINE, compileExpression(testCase.root, null))

      const expectedWorldVolume = expectedModelVolume * AFFINE_ABS_DET
      for (const [label, geometry] of [['operands transformed before boolean', operandFirst],
        ['boolean result transformed after boolean', resultFirst]]) {
        const polygons = geom3.toPolygons(geometry)
        const volume = measurements.measureVolume(geometry)
        assert.ok(
          closeEnough(volume, expectedWorldVolume),
          describeFailure(testCase, `${label}; ${TRANSFORM_LABEL}`,
            `world volume ${volume} differs from reference ${expectedWorldVolume} ` +
            `(diff ${Math.abs(volume - expectedWorldVolume)})`)
        )
        assert.equal(polygons.length === 0, testCase.empty,
          describeFailure(testCase, label, 'polygon presence disagrees with reference emptiness'))
        for (const probe of testCase.probes) {
          const worldProbe = transformPoint(AFFINE, probe.p)
          castProbe(polygons, worldProbe, probe.inside,
            describeFailure(testCase, `${label}; ${TRANSFORM_LABEL}`, probe.note))
        }
      }

      // The two world pipelines must occupy the same space even though their
      // tessellations may differ; triangle counts are deliberately not compared.
      const left = geom3.toPolygons(operandFirst)
      const right = geom3.toPolygons(resultFirst)
      for (const probe of testCase.probes) {
        const worldProbe = transformPoint(AFFINE, probe.p)
        const inLeft = pointInPolygons(left, worldProbe)
        const inRight = pointInPolygons(right, worldProbe)
        assert.equal(inLeft, inRight,
          describeFailure(testCase, 'pipeline comparison',
            `probe (${worldProbe.join(', ')}) verdicts differ: operands-first ${inLeft} vs result-first ${inRight}`))
      }
    })
  }
})

test('boolean-cavity-winding', async (t) => {
  for (const testCase of cases) {
    await t.test(testCase.name, () => {
      const expectedModelVolume = referenceVolume(testCase.root)
      const expectedWorldVolume = expectedModelVolume * AFFINE_ABS_DET

      const checks = [
        ['identity transform', compileExpression(testCase.root, null), expectedModelVolume],
        [TRANSFORM_LABEL, compileExpression(testCase.root, AFFINE), expectedWorldVolume],
        [`post-boolean ${TRANSFORM_LABEL}`,
          transforms.transform(AFFINE, compileExpression(testCase.root, null)), expectedWorldVolume]
      ]
      for (const [label, geometry, expected] of checks) {
        const polygons = geom3.toPolygons(geometry)
        if (testCase.empty) {
          assert.equal(polygons.length, 0, describeFailure(testCase, label, 'expected no polygons'))
          continue
        }
        const signed = signedVolumeOfPolygons(polygons)
        // A closed outward-oriented surface sums to a positive value equal to
        // the occupied volume; flipped cavity walls would inflate it and a
        // fully inverted shell would make it negative.
        assert.ok(signed > 0,
          describeFailure(testCase, label, `signed volume ${signed} is not outward-oriented`))
        assert.ok(closeEnough(signed, expected),
          describeFailure(testCase, label,
            `independently accumulated signed volume ${signed} differs from occupied reference ${expected} ` +
            `(diff ${Math.abs(signed - expected)})`))
      }

      // Dedicated cavity check: the inner wall subtracts exactly the cavity
      // volume, so outer box minus inner box must match the grid reference.
      if (testCase.name === 'sealed-cavity') {
        const polygons = geom3.toPolygons(compileExpression(testCase.root, null))
        const signed = signedVolumeOfPolygons(polygons)
        assert.ok(closeEnough(signed, expectedModelVolume),
          describeFailure(testCase, 'cavity winding probe',
            `inner wall orientation wrong: signed ${signed} vs reference ${expectedModelVolume}`))
      }
    })
  }
})

test('boolean-lazy-affine', async (t) => {
  for (const testCase of cases) {
    await t.test(testCase.name, () => {
      const expectedWorldVolume = referenceVolume(testCase.root) * AFFINE_ABS_DET

      // Reference run: operands still carry their lazy affine matrices.
      const freshLeaves = () => collectLeaves(testCase.root, AFFINE)
      const first = compileWithLeaves(testCase.root, freshLeaves())
      const firstPolygons = geom3.toPolygons(first)
      const firstVolume = measurements.measureVolume(first)
      assert.ok(closeEnough(firstVolume, expectedWorldVolume),
        describeFailure(testCase, TRANSFORM_LABEL, `baseline volume ${firstVolume} vs ${expectedWorldVolume}`))

      // Reuse (a): measureVolume materializes each operand's lazy matrix;
      // re-running the same boolean expression on the reused objects must
      // produce the same occupied space.
      const leavesMeasured = freshLeaves()
      leavesMeasured.forEach((geometry) => measurements.measureVolume(geometry))
      const rerunAfterMeasure = compileWithLeaves(testCase.root, leavesMeasured)
      assert.ok(
        closeEnough(measurements.measureVolume(rerunAfterMeasure), firstVolume),
        describeFailure(testCase, 'reuse after measureVolume', 'volume changed when operands were reused'))
      for (const probe of testCase.probes) {
        const worldProbe = transformPoint(AFFINE, probe.p)
        castProbe(geom3.toPolygons(rerunAfterMeasure), worldProbe, probe.inside,
          describeFailure(testCase, 'reuse after measureVolume', probe.note))
      }

      // Reuse (b): explicit geom3.toPolygons materialization on the operands.
      const leavesExplicit = freshLeaves()
      leavesExplicit.forEach((geometry) => geom3.toPolygons(geometry))
      const rerunAfterToPolygons = compileWithLeaves(testCase.root, leavesExplicit)
      assert.ok(
        closeEnough(measurements.measureVolume(rerunAfterToPolygons), firstVolume),
        describeFailure(testCase, 'reuse after geom3.toPolygons', 'volume changed when operands were reused'))
      for (const probe of testCase.probes) {
        const worldProbe = transformPoint(AFFINE, probe.p)
        castProbe(geom3.toPolygons(rerunAfterToPolygons), worldProbe, probe.inside,
          describeFailure(testCase, 'reuse after geom3.toPolygons', probe.note))
      }

      // Reusing the already-materialized operands a second time must stay stable.
      const rerunAgain = compileWithLeaves(testCase.root, leavesExplicit)
      assert.ok(
        closeEnough(measurements.measureVolume(rerunAgain), firstVolume),
        describeFailure(testCase, 'second reuse after materialization', 'geometry drifted between runs'))

      if (testCase.empty) {
        assert.equal(firstPolygons.length, 0,
          describeFailure(testCase, TRANSFORM_LABEL, 'empty reference result should have no polygons'))
      } else {
        assert.ok(firstPolygons.length > 0,
          describeFailure(testCase, TRANSFORM_LABEL, 'non-empty reference result should have polygons'))
      }
    })
  }
})
