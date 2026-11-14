const assert = require('node:assert/strict')
const j = require('../packages/modeling/src')
const a = j.primitives.cuboid({ size: [2, 2, 2] })
const b = j.primitives.cuboid({ size: [2, 2, 2], center: [1, 0, 0] })
for (const [name, expected] of [['union', 12], ['intersect', 4], ['subtract', 4]]) {
  const r = j.booleans[name](a, b)
  const v = j.measurements.measureVolume(r)
  assert.ok(Math.abs(v - expected) < 1e-10, `${name}: ${v}`)
  console.log(`NORMAL ${name} volume=${v}`)
}
if (!process.argv.includes('--normal-only')) {
  const thickness = 2 ** -20
  const slab = j.primitives.cuboid({ size: [3, 3, thickness] })
  const r = j.booleans.intersect(j.primitives.cuboid({ size: [2, 2, 2] }), slab)
  const actual = j.measurements.measureVolume(r)
  const expected = 4 * thickness
  const relativeError = Math.abs(actual / expected - 1)
  console.log(`THIN_INTERSECTION polygons=${r.polygons.length} actual=${actual} expected=${expected} relative_error=${relativeError}`)
  assert.equal(r.polygons.length, 4, 'baseline records the original missing-side phenomenon')
  assert.ok(relativeError > 0.08 && relativeError < 0.09)
  console.log('ORIGINAL_THIN_DEFECT_REPRODUCED')
}
console.log('JSCAD_BASELINE_OK')
