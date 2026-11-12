import Delaunator from '../index.js';
import assert from 'node:assert/strict';
const d=Delaunator.from([[0,0],[4,0],[4,4],[0,4],[1,1],[3,1],[2,3],[2,2]]);
assert.equal(d.hull.length,4);
assert.equal(d.triangles.length,30);
for(let e=0;e<d.halfedges.length;e++) {
 const t=d.halfedges[e];
 if(t>=0)assert.equal(d.halfedges[t],e);
}
d.coords[8]=1.2;d.update();
assert.equal(d.triangles.length,30);
console.log('BASELINE triangulate-update PASS triangles=10 hull=4');
