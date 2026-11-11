const assert = require('node:assert/strict')
const m = require('../packages/modeling/src')
const a = m.primitives.cuboid({size:[4,4,4]})
const b = m.primitives.cuboid({size:[2,2,6]})
const near = (a,b) => assert.ok(Math.abs(a-b)<1e-8,`${a} != ${b}`)
const cases = [['union',72],['intersect',16],['subtract',48]]
for (const [op,v] of cases) {
  const r = m.booleans[op](a,b)
  near(m.measurements.measureVolume(r),v)
  assert.ok(m.geometries.geom3.toPolygons(r).length>0)
  console.log(`JSCAD ${op} volume ${v} PASS`)
}
const x = m.transforms.translate([3,-2,1],m.transforms.mirrorX(m.transforms.scale([2,3,1],a)))
near(m.measurements.measureVolume(x),384)
console.log('JSCAD reflected lazy transform volume 384 PASS')
