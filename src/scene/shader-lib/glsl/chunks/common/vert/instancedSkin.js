export default /* glsl */`

attribute vec4 vertex_boneWeights;
attribute vec4 vertex_boneIndices;

attribute vec4 instance_line1;
attribute vec4 instance_line2;
attribute vec4 instance_line3;
attribute vec4 instance_line4;

// the pose atlas of all instances, and the number of bones of one instance
uniform highp sampler2D texture_instancedSkinPoseMap;
uniform float instancedSkin_boneCount;

// the model matrix of one instance of the drawn mesh, relative to the mesh instance node
mat4 getInstanceMatrix() {
    return mat4(instance_line1, instance_line2, instance_line3, instance_line4);
}

void getInstancedSkinBoneMatrix(const in int width, const in int index, out vec4 v1, out vec4 v2, out vec4 v3) {

    int v = index / width;
    int u = index % width;

    v1 = texelFetch(texture_instancedSkinPoseMap, ivec2(u + 0, v), 0);
    v2 = texelFetch(texture_instancedSkinPoseMap, ivec2(u + 1, v), 0);
    v3 = texelFetch(texture_instancedSkinPoseMap, ivec2(u + 2, v), 0);
}

// returns the skinning matrix of one vertex, addressing the bone block of the current instance
mat4 getInstancedSkinMatrix(const in vec4 indicesFloat, const in vec4 weights) {

    int width = textureSize(texture_instancedSkinPoseMap, 0).x;

    // the bone block of this instance starts after the blocks of the previous instances
    int instanceBase = gl_InstanceID * int(instancedSkin_boneCount + 0.5);

    // texel address of each bone, every bone starts at a multiple of three
    ivec4 indices = (ivec4(indicesFloat + 0.5) + ivec4(instanceBase)) * 3;

    vec4 a1, a2, a3;
    getInstancedSkinBoneMatrix(width, indices.x, a1, a2, a3);

    vec4 b1, b2, b3;
    getInstancedSkinBoneMatrix(width, indices.y, b1, b2, b3);

    vec4 c1, c2, c3;
    getInstancedSkinBoneMatrix(width, indices.z, c1, c2, c3);

    vec4 d1, d2, d3;
    getInstancedSkinBoneMatrix(width, indices.w, d1, d2, d3);

    // blend the four 4x3 matrices
    vec4 v1 = a1 * weights.x + b1 * weights.y + c1 * weights.z + d1 * weights.w;
    vec4 v2 = a2 * weights.x + b2 * weights.y + c2 * weights.z + d2 * weights.w;
    vec4 v3 = a3 * weights.x + b3 * weights.y + c3 * weights.z + d3 * weights.w;

    float one = dot(weights, vec4(1.0));

    // transpose to a 4x4 matrix
    return mat4(
        v1.x, v2.x, v3.x, 0,
        v1.y, v2.y, v3.y, 0,
        v1.z, v2.z, v3.z, 0,
        v1.w, v2.w, v3.w, one
    );
}
`;
