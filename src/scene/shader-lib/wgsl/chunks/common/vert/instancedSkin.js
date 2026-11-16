export default /* wgsl */`

attribute vertex_boneWeights: vec4f;
attribute vertex_boneIndices: vec4f;

attribute instance_line1: vec4f;
attribute instance_line2: vec4f;
attribute instance_line3: vec4f;
attribute instance_line4: vec4f;

// the pose atlas of all instances, and the number of bones of one instance
var texture_instancedSkinPoseMap: texture_2d<uff>;
uniform instancedSkin_boneCount: f32;

// the model matrix of one instance of the drawn mesh, relative to the mesh instance node
fn getInstanceMatrix() -> mat4x4f {
    return mat4x4f(instance_line1, instance_line2, instance_line3, instance_line4);
}

struct InstancedSkinBoneMatrix {
    v1: vec4f,
    v2: vec4f,
    v3: vec4f,
}

fn getInstancedSkinBoneMatrix(width: i32, index: i32) -> InstancedSkinBoneMatrix {

    let v = index / width;
    let u = index % width;

    var result: InstancedSkinBoneMatrix;
    result.v1 = textureLoad(texture_instancedSkinPoseMap, vec2i(u + 0, v), 0);
    result.v2 = textureLoad(texture_instancedSkinPoseMap, vec2i(u + 1, v), 0);
    result.v3 = textureLoad(texture_instancedSkinPoseMap, vec2i(u + 2, v), 0);
    return result;
}

// returns the skinning matrix of one vertex, addressing the bone block of the current instance
fn getInstancedSkinMatrix(indicesFloat: vec4f, weights: vec4f) -> mat4x4f {

    let width = i32(textureDimensions(texture_instancedSkinPoseMap).x);

    // the bone block of this instance starts after the blocks of the previous instances
    let instanceBase = i32(pcInstanceIndex) * i32(uniform.instancedSkin_boneCount + 0.5);

    // texel address of each bone, every bone starts at a multiple of three
    let indices = (vec4i(indicesFloat + 0.5) + vec4i(instanceBase)) * 3;

    let boneA = getInstancedSkinBoneMatrix(width, indices.x);
    let boneB = getInstancedSkinBoneMatrix(width, indices.y);
    let boneC = getInstancedSkinBoneMatrix(width, indices.z);
    let boneD = getInstancedSkinBoneMatrix(width, indices.w);

    // blend the four 4x3 matrices
    let v1 = boneA.v1 * weights.x + boneB.v1 * weights.y + boneC.v1 * weights.z + boneD.v1 * weights.w;
    let v2 = boneA.v2 * weights.x + boneB.v2 * weights.y + boneC.v2 * weights.z + boneD.v2 * weights.w;
    let v3 = boneA.v3 * weights.x + boneB.v3 * weights.y + boneC.v3 * weights.z + boneD.v3 * weights.w;

    let one = dot(weights, vec4f(1.0, 1.0, 1.0, 1.0));

    // transpose to a 4x4 matrix
    return mat4x4f(
        v1.x, v2.x, v3.x, 0,
        v1.y, v2.y, v3.y, 0,
        v1.z, v2.z, v3.z, 0,
        v1.w, v2.w, v3.w, one
    );
}
`;
