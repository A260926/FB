import assert from 'node:assert/strict';
import Scene from '../src/Scene.js';
import {triangle,mesh,cameraAt,picker} from './fixtures.mjs';
const scene=new Scene(),object=mesh(triangle());scene.add(object);scene.update();
const camera=cameraAt(),list=scene.updateRenderList(camera),hits=picker(scene,camera).pickAll(50,50);
assert.equal(list.opaque.length,1);assert.equal(list.opaque[0],object);
assert.equal(hits.length,1);assert.equal(hits[0].target,object);
assert.ok(Math.abs(hits[0].pointWorld.array[0]-.25)<1e-5);
console.log('static_visible=1 static_hits=1');
