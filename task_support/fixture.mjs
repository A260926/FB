import {Geometry} from '../src/core/Geometry.js';
import {Transform} from '../src/core/Transform.js';
import {Mat4} from '../src/math/Mat4.js';
import {GLTFSkin} from '../src/extras/GLTFSkin.js';
export function makeDevice() {
    let serial=0, bound=null;
    const uploads=[];
    const gl={canvas:{}, uploads,
        FLOAT:5126, UNSIGNED_SHORT:5123, UNSIGNED_INT:5125, UNSIGNED_BYTE:5121,
        ARRAY_BUFFER:34962, ELEMENT_ARRAY_BUFFER:34963, STATIC_DRAW:35044,
        TRIANGLES:4, TEXTURE_2D:3553, RGBA:6408, RGBA32F:34836, NEAREST:9728,
        CLAMP_TO_EDGE:33071, LINEAR:9729, NEAREST_MIPMAP_LINEAR:9986, REPEAT:10497,
        createBuffer(){return {deviceSerial:++serial};},
        bindBuffer(target,buffer){bound=buffer;},
        bufferData(target,data,usage){uploads.push({buffer:bound,target,bytes:Buffer.from(new Uint8Array(data.buffer,data.byteOffset,data.byteLength))});},
        bufferSubData(target,offset,data){uploads.push({buffer:bound,target,offset,bytes:Buffer.from(new Uint8Array(data.buffer,data.byteOffset,data.byteLength))});},
        createTexture(){return {deviceSerial:++serial};},
        renderer:{isWebgl2:true,parameters:{maxAnisotropy:1},state:{boundBuffer:null,textureUnits:[],activeTextureUnit:0},bindVertexArray(){},currentGeometry:null}
    };
    return gl;
}
export function makeRig(){
    const gl=makeDevice();
    const geometry=new Geometry(gl,{
        position:{size:3,data:new Float32Array([2,0,0,3,0,0,2,1,0])},
        index:{data:new Uint16Array([0,1,2])},
        skinIndex:{size:4,data:new Float32Array(12)},
        skinWeight:{size:4,data:new Float32Array([1,0,0,0,1,0,0,0,1,0,0,0])}
    });
    const root=new Transform(),bone=new Transform();
    root.position.x=1;bone.position.x=2;bone.setParent(root);
    bone.bindInverse=new Mat4().translate([-2,0,0]);
    const program={uniforms:{boneTexture:{value:null},boneTextureSize:{value:0}}};
    const skin=new GLTFSkin(gl,{skeleton:{joints:[bone]},geometry,program});
    return {gl,geometry,root,bone,skin};
}

// Programmatic source rig: two influences, animated helper and noncommuting axes.
// This fixture creates inputs only. Reference math lives in baseline.mjs.
export function makeEditingRig(){
    const gl=makeDevice();
    const geometry=new Geometry(gl,{
        position:{size:3,data:new Float32Array([1,0,0,0,2,0,-1,0,1])},
        index:{data:new Uint16Array([0,1,2])},
        skinIndex:{size:4,data:new Float32Array([0,1,0,0,0,1,0,0,0,1,0,0])},
        skinWeight:{size:4,data:new Float32Array([.8,.2,0,0,.25,.75,0,0,.5,.5,0,0])}
    });
    const root=new Transform(), helper=new Transform(), a=new Transform(), b=new Transform();
    root.position.set(2,-1,.5);root.rotation.z=.3;
    helper.position.set(1,0,0);helper.setParent(root);
    a.position.set(.4,0,0);a.setParent(helper);b.position.set(0,1,0);b.setParent(a);
    a.bindInverse=new Mat4().translate([-.3,-.2,0]);
    b.bindInverse=new Mat4().translate([-.3,-1.2,0]);
    const program={uniforms:{boneTexture:{value:null},boneTextureSize:{value:0}}};
    const skin=new GLTFSkin(gl,{skeleton:{joints:[a,b]},geometry,program});
    skin.position.x=99;skin.setParent(root);
    return {gl,geometry,root,helper,a,b,skin};
}
