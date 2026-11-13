import Geometry from '../src/Geometry.js';
import Scene from '../src/Scene.js';
import Mesh from '../src/Mesh.js';
import Node from '../src/Node.js';
import Joint from '../src/Joint.js';
import Skeleton from '../src/Skeleton.js';
import Perspective from '../src/camera/Perspective.js';
import Vector3 from '../src/math/Vector3.js';
import Matrix4 from '../src/math/Matrix4.js';
import RayPicking from '../src/picking/RayPicking.js';
import Renderer from '../src/Renderer.js';
export function triangle(weights=[1,0,0],joints=[0,1,0,1]) {
  const g=new Geometry();
  g.attributes.position.fromArray([[0,0,0],[1,0,0],[0,1,0]]);
  g.attributes.weight.fromArray([weights,weights,weights]);
  g.attributes.joint.fromArray([joints,joints,joints]);
  g.initIndicesFromArray([[0,1,2]]);g.updateBoundingBox();return g;
}
export function mesh(g) {
  // Scene and CPU picking need a shader descriptor, not a GPU program.
  return new Mesh({geometry:g,material:{shader:{},transparent:false},culling:false});
}
export function cameraAt(x=.25,y=.25,z=2) {
  const camera=new Perspective({near:.01,far:100,aspect:1,fov:60});
  camera.position.set(x,y,z);camera.lookAt(new Vector3(x,y,0));camera.update();return camera;
}
export function picker(scene,camera) {
  const renderer={_height:100,viewport:{x:0,y:0,width:100,height:100},screenToNDC:Renderer.prototype.screenToNDC};
  return new RayPicking({scene,camera,renderer});
}
export function rig() {
  const scene=new Scene(),skeleton=new Skeleton();
  skeleton.joints=[0,1,2,3].map(index=>new Joint({index,node:new Node()}));
  for(const j of skeleton.joints)j.node.update();
  skeleton.updateJointMatrices();
  skeleton.joints[3].node.position.set(10,0,0);skeleton.joints[3].node.update();
  const geometry=triangle([0,0,0]);
  skeleton.updateJointsBoundingBoxes(geometry);skeleton.update();
  const a=mesh(geometry),b=mesh(geometry);
  for(const m of [a,b]){m.skeleton=skeleton;m.joints=[0,3];m.offsetMatrix=new Matrix4();scene.add(m);}
  a.offsetMatrix.array[12]=20;b.offsetMatrix.array[12]=-20;
  scene.update();return {scene,skeleton,geometry,a,b};
}
