#ifndef TESTS_IQM_FIXTURE_H
#define TESTS_IQM_FIXTURE_H

// iqm_fixture.h - programmatic IQM (version 2) file construction plus an
// independent matrix reference implementation, shared by
// tests/async_pose.cpp and example/async_pose.cpp.
//
// The builder writes a complete, well-formed IQM file image (mesh + one
// animation) from a plain-data Rig description.  Animation channels are
// quantized to the 16-bit channel encoding exactly like a real exporter,
// and the builder reports the decoded (effective) pose values so reference
// computations start from the very same floats the loader will see.
//
// The reference evaluator re-derives, independently of the production code
// in demo/async_pose.cpp, the base/inverse-bind poses, the per-frame
// pre-concatenated pose matrices, the adjacent-frame interpolation with
// parent-chain concatenation, and the four-attribute skinning with the
// adjoint-transpose direction transform.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "demo/geom.h"
#include "demo/iqm.h"

namespace fixture
{

struct JointSpec
{
    int parent;          // -1 for a root; parents must come before children
    Vec3 translate;      // bind pose, stored exactly as floats
    Quat rotate;         // bind rotation, need not be normalized
    Vec3 scale;          // bind scale, may be non-uniform
};

struct PoseKey
{
    Vec3 translate;      // one joint's local pose at one frame
    Quat rotate;
    Vec3 scale;
};

struct VertexSpec
{
    Vec3 position;
    Vec3 normal;
    Vec4 tangent;        // w holds the bitangent sign
    unsigned char index[4];
    unsigned char weight[4];
};

struct Rig
{
    std::vector<JointSpec> joints;
    std::vector<std::vector<PoseKey>> frames; // [frame][joint]
    std::vector<VertexSpec> vertices;
};

// Channel encodings used for the quantized animation data.
inline float channel_offset(int channel) { return channel < 3 ? -8.0f : (channel < 7 ? -1.0f : 0.0f); }
inline float channel_scale(int channel)  { return channel < 3 ? 16.0f/65535.0f : (channel < 7 ? 2.0f/65535.0f : 4.0f/65535.0f); }

inline float channel_value(const PoseKey &key, int channel)
{
    switch(channel)
    {
    case 0: return key.translate.x;
    case 1: return key.translate.y;
    case 2: return key.translate.z;
    case 3: return key.rotate.x;
    case 4: return key.rotate.y;
    case 5: return key.rotate.z;
    case 6: return key.rotate.w;
    case 7: return key.scale.x;
    case 8: return key.scale.y;
    default: return key.scale.z;
    }
}

inline void set_channel_value(PoseKey &key, int channel, float value)
{
    switch(channel)
    {
    case 0: key.translate.x = value; break;
    case 1: key.translate.y = value; break;
    case 2: key.translate.z = value; break;
    case 3: key.rotate.x = value; break;
    case 4: key.rotate.y = value; break;
    case 5: key.rotate.z = value; break;
    case 6: key.rotate.w = value; break;
    case 7: key.scale.x = value; break;
    case 8: key.scale.y = value; break;
    default: key.scale.z = value; break;
    }
}

// Decodes a quantized channel value with the same float arithmetic the
// loader uses (offset + data * scale).
inline float decode_channel(int channel, unsigned short data)
{
    return channel_offset(channel) + (float)data * channel_scale(channel);
}

struct BuiltRig
{
    Rig rig;                                   // input description
    std::vector<unsigned char> filebytes;      // complete IQM file image
    std::vector<iqmjoint> joints;              // as stored in the file
    std::vector<std::vector<PoseKey>> frames;  // decoded (effective) poses
};

// Builds the IQM file image for a rig.  Every animation channel is exported
// with mask 0x3FF (all ten channels present per pose).
inline BuiltRig build_rig(const Rig &rig)
{
    BuiltRig built;
    built.rig = rig;
    const unsigned numjoints = (unsigned)rig.joints.size();
    const unsigned numframes = (unsigned)rig.frames.size();
    const unsigned numverts = (unsigned)rig.vertices.size();
    const unsigned numchannels = 10 * numjoints;

    built.frames = rig.frames;
    std::vector<unsigned short> framedata((size_t)numframes * numchannels);
    for(unsigned f = 0; f < numframes; f++)
        for(unsigned j = 0; j < numjoints; j++)
            for(int c = 0; c < 10; c++)
            {
                const float offset = channel_offset(c), scale = channel_scale(c);
                float want = channel_value(rig.frames[f][j], c);
                long q = lround((want - offset) / scale);
                if(q < 0) q = 0;
                if(q > 65535) q = 65535;
                framedata[(size_t)f * numchannels + j * 10 + c] = (unsigned short)q;
                // Report the effective value so references use the decoded pose.
                set_channel_value(built.frames[f][j], c, decode_channel(c, (unsigned short)q));
            }

    built.joints.resize(numjoints);
    for(unsigned j = 0; j < numjoints; j++)
    {
        iqmjoint &out = built.joints[j];
        memset(&out, 0, sizeof(out));
        out.name = 0;
        out.parent = rig.joints[j].parent;
        out.translate[0] = rig.joints[j].translate.x;
        out.translate[1] = rig.joints[j].translate.y;
        out.translate[2] = rig.joints[j].translate.z;
        out.rotate[0] = rig.joints[j].rotate.x;
        out.rotate[1] = rig.joints[j].rotate.y;
        out.rotate[2] = rig.joints[j].rotate.z;
        out.rotate[3] = rig.joints[j].rotate.w;
        out.scale[0] = rig.joints[j].scale.x;
        out.scale[1] = rig.joints[j].scale.y;
        out.scale[2] = rig.joints[j].scale.z;
    }

    // Lay out the file image: header, vertex arrays, vertex data, one
    // triangle + mesh, joints, poses, one anim, frame channel data.
    const unsigned numarrays = 5;
    size_t offset = sizeof(iqmheader);
    const size_t ofs_vertexarrays = offset; offset += numarrays * sizeof(iqmvertexarray);
    const size_t ofs_position = offset;  offset += 3 * numverts * sizeof(float);
    const size_t ofs_normal = offset;    offset += 3 * numverts * sizeof(float);
    const size_t ofs_tangent = offset;   offset += 4 * numverts * sizeof(float);
    const size_t ofs_blendindex = offset;  offset += 4 * numverts;
    const size_t ofs_blendweight = offset; offset += 4 * numverts;
    const size_t ofs_triangles = offset; offset += sizeof(iqmtriangle);
    const size_t ofs_meshes = offset;    offset += sizeof(iqmmesh);
    const size_t ofs_joints = offset;    offset += numjoints * sizeof(iqmjoint);
    const size_t ofs_poses = offset;     offset += numjoints * sizeof(iqmpose);
    const size_t ofs_anims = offset;     offset += sizeof(iqmanim);
    const size_t ofs_frames = offset;    offset += framedata.size() * sizeof(unsigned short);

    built.filebytes.resize(offset);
    unsigned char *buf = built.filebytes.data();

    iqmheader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, IQM_MAGIC, sizeof(hdr.magic));
    hdr.version = IQM_VERSION;
    hdr.filesize = (unsigned)offset;
    hdr.flags = 0;
    hdr.num_text = 0; hdr.ofs_text = 0;
    hdr.num_meshes = 1; hdr.ofs_meshes = (unsigned)ofs_meshes;
    hdr.num_vertexarrays = numarrays;
    hdr.num_vertexes = numverts;
    hdr.ofs_vertexarrays = (unsigned)ofs_vertexarrays;
    hdr.num_triangles = 1;
    hdr.ofs_triangles = (unsigned)ofs_triangles;
    hdr.ofs_adjacency = 0;
    hdr.num_joints = numjoints; hdr.ofs_joints = (unsigned)ofs_joints;
    hdr.num_poses = numjoints;  hdr.ofs_poses = (unsigned)ofs_poses;
    hdr.num_anims = 1;          hdr.ofs_anims = (unsigned)ofs_anims;
    hdr.num_frames = numframes;
    hdr.num_framechannels = numchannels;
    hdr.ofs_frames = (unsigned)ofs_frames;
    hdr.ofs_bounds = 0;
    hdr.num_comment = 0; hdr.ofs_comment = 0;
    hdr.num_extensions = 0; hdr.ofs_extensions = 0;
    memcpy(buf, &hdr, sizeof(hdr));

    iqmvertexarray *vas = (iqmvertexarray *)(buf + ofs_vertexarrays);
    vas[0] = iqmvertexarray{ IQM_POSITION, 0, IQM_FLOAT, 3, (unsigned)ofs_position };
    vas[1] = iqmvertexarray{ IQM_NORMAL, 0, IQM_FLOAT, 3, (unsigned)ofs_normal };
    vas[2] = iqmvertexarray{ IQM_TANGENT, 0, IQM_FLOAT, 4, (unsigned)ofs_tangent };
    vas[3] = iqmvertexarray{ IQM_BLENDINDEXES, 0, IQM_UBYTE, 4, (unsigned)ofs_blendindex };
    vas[4] = iqmvertexarray{ IQM_BLENDWEIGHTS, 0, IQM_UBYTE, 4, (unsigned)ofs_blendweight };

    float *pos = (float *)(buf + ofs_position), *nrm = (float *)(buf + ofs_normal),
          *tan = (float *)(buf + ofs_tangent);
    unsigned char *bidx = buf + ofs_blendindex, *bwgt = buf + ofs_blendweight;
    for(unsigned v = 0; v < numverts; v++)
    {
        const VertexSpec &vert = rig.vertices[v];
        pos[3*v+0] = vert.position.x; pos[3*v+1] = vert.position.y; pos[3*v+2] = vert.position.z;
        nrm[3*v+0] = vert.normal.x;   nrm[3*v+1] = vert.normal.y;   nrm[3*v+2] = vert.normal.z;
        tan[4*v+0] = vert.tangent.x;  tan[4*v+1] = vert.tangent.y;
        tan[4*v+2] = vert.tangent.z;  tan[4*v+3] = vert.tangent.w;
        for(int k = 0; k < 4; k++) { bidx[4*v+k] = vert.index[k]; bwgt[4*v+k] = vert.weight[k]; }
    }

    iqmtriangle *tri = (iqmtriangle *)(buf + ofs_triangles);
    tri->vertex[0] = 0; tri->vertex[1] = numverts > 1 ? 1 : 0; tri->vertex[2] = numverts > 2 ? 2 : 0;

    iqmmesh *mesh = (iqmmesh *)(buf + ofs_meshes);
    mesh->name = 0; mesh->material = 0;
    mesh->first_vertex = 0; mesh->num_vertexes = numverts;
    mesh->first_triangle = 0; mesh->num_triangles = 1;

    memcpy(buf + ofs_joints, built.joints.data(), numjoints * sizeof(iqmjoint));

    iqmpose *poses = (iqmpose *)(buf + ofs_poses);
    for(unsigned j = 0; j < numjoints; j++)
    {
        iqmpose &p = poses[j];
        p.parent = rig.joints[j].parent;
        p.mask = 0x3FF;
        for(int c = 0; c < 10; c++)
        {
            p.channeloffset[c] = channel_offset(c);
            p.channelscale[c] = channel_scale(c);
        }
    }

    iqmanim *anim = (iqmanim *)(buf + ofs_anims);
    anim->name = 0;
    anim->first_frame = 0;
    anim->num_frames = numframes;
    anim->framerate = 30.0f;
    anim->flags = IQM_LOOP;

    memcpy(buf + ofs_frames, framedata.data(), framedata.size() * sizeof(unsigned short));

    return built;
}

// Writes a file image to disk; returns false on failure.
inline bool write_file(const std::string &path, const std::vector<unsigned char> &bytes)
{
    FILE *f = fopen(path.c_str(), "wb");
    if(!f) return false;
    bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Independent reference implementation.
// ---------------------------------------------------------------------------

struct ReferenceRig
{
    BuiltRig built;
    std::vector<Matrix3x4> baseframe;
    std::vector<Matrix3x4> inversebase;
    std::vector<Matrix3x4> frames; // numframes * numjoints, pose * inverse-bind
};

// Computes base/inverse-bind poses and the pre-concatenated frame matrices
// from the decoded animation data, independently of the production loader.
inline ReferenceRig prepare_rig(const Rig &rig)
{
    ReferenceRig ref;
    ref.built = build_rig(rig);
    const unsigned numjoints = (unsigned)rig.joints.size();
    const unsigned numframes = (unsigned)rig.frames.size();

    ref.baseframe.resize(numjoints);
    ref.inversebase.resize(numjoints);
    for(unsigned j = 0; j < numjoints; j++)
    {
        const iqmjoint &joint = ref.built.joints[j];
        ref.baseframe[j] = Matrix3x4(Quat(joint.rotate).normalize(), Vec3(joint.translate), Vec3(joint.scale));
        ref.inversebase[j].invert(ref.baseframe[j]);
        if(joint.parent >= 0)
        {
            ref.baseframe[j] = ref.baseframe[joint.parent] * ref.baseframe[j];
            ref.inversebase[j] *= ref.inversebase[joint.parent];
        }
    }

    ref.frames.resize((size_t)numframes * numjoints);
    for(unsigned f = 0; f < numframes; f++)
        for(unsigned j = 0; j < numjoints; j++)
        {
            const PoseKey &key = ref.built.frames[f][j];
            Matrix3x4 m(Quat(key.rotate).normalize(), key.translate, key.scale);
            int parent = rig.joints[j].parent;
            if(parent >= 0) ref.frames[(size_t)f * numjoints + j] = ref.baseframe[parent] * m * ref.inversebase[j];
            else ref.frames[(size_t)f * numjoints + j] = m * ref.inversebase[j];
        }
    return ref;
}

struct ReferenceOutput
{
    std::vector<Matrix3x4> joints;
    std::vector<Vec3> positions, normals, tangents, bitangents;
};

// Evaluates the pose at an absolute frame number with an independent
// positive-modulo wrap and the reference skinning formulas.
inline ReferenceOutput evaluate_reference(const ReferenceRig &ref, float curframe)
{
    const int numjoints = (int)ref.built.rig.joints.size();
    const int numframes = (int)ref.built.rig.frames.size();
    const int numverts = (int)ref.built.rig.vertices.size();

    double floored = floor((double)curframe);
    float frameoffset = (float)((double)curframe - floored);
    double wrapped = fmod(floored, (double)numframes);
    if(wrapped < 0) wrapped += numframes;
    int frame1 = (int)wrapped;
    int frame2 = frame1 + 1 < numframes ? frame1 + 1 : 0;

    ReferenceOutput out;
    out.joints.resize(numjoints);
    const Matrix3x4 *mat1 = &ref.frames[(size_t)frame1 * numjoints];
    const Matrix3x4 *mat2 = &ref.frames[(size_t)frame2 * numjoints];
    for(int i = 0; i < numjoints; i++)
    {
        Matrix3x4 mat = mat1[i]*(1 - frameoffset) + mat2[i]*frameoffset;
        int parent = ref.built.joints[i].parent;
        if(parent >= 0) out.joints[i] = out.joints[parent] * mat;
        else out.joints[i] = mat;
    }

    out.positions.resize(numverts);
    out.normals.resize(numverts);
    out.tangents.resize(numverts);
    out.bitangents.resize(numverts);
    for(int i = 0; i < numverts; i++)
    {
        const VertexSpec &vert = ref.built.rig.vertices[i];
        Matrix3x4 mat = out.joints[vert.index[0]] * (vert.weight[0]/255.0f);
        for(int j = 1; j < 4 && vert.weight[j]; j++)
            mat += out.joints[vert.index[j]] * (vert.weight[j]/255.0f);

        out.positions[i] = mat.transform(vert.position);
        Matrix3x3 matnorm(mat.b.cross3(mat.c), mat.c.cross3(mat.a), mat.a.cross3(mat.b));
        out.normals[i] = matnorm.transform(vert.normal);
        out.tangents[i] = matnorm.transform(Vec3(vert.tangent));
        out.bitangents[i] = out.normals[i].cross(out.tangents[i]) * vert.tangent.w;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Two ready-made, deliberately different rigs.
// ---------------------------------------------------------------------------

// Three-joint parent-child skeleton with a non-unit bind pose, non-uniform
// scale, four animation frames and multi-bone vertex weights.
inline Rig make_rig_alpha()
{
    Rig rig;
    rig.joints = {
        { -1, Vec3(0.5f, -0.25f, 1.0f),  Quat(0.1f, 0.2f, -0.15f, 0.95f), Vec3(1.0f, 2.0f, 0.5f) },
        {  0, Vec3(0.0f, 1.5f, 0.25f),   Quat(-0.2f, 0.05f, 0.3f, 0.9f),  Vec3(0.75f, 1.25f, 1.0f) },
        {  1, Vec3(0.25f, 0.0f, 0.75f),  Quat(0.15f, -0.25f, 0.1f, 0.85f), Vec3(1.5f, 0.5f, 1.25f) },
    };
    rig.frames = {
        { { Vec3(0.0f, 0.0f, 0.0f),   Quat(0.0f, 0.0f, 0.0f, 1.0f),   Vec3(1.0f, 1.0f, 1.0f) },
          { Vec3(0.0f, 0.5f, 0.0f),   Quat(0.1f, 0.0f, 0.0f, 0.995f), Vec3(1.0f, 1.0f, 1.0f) },
          { Vec3(0.0f, 0.0f, 0.0f),   Quat(0.0f, 0.1f, 0.0f, 0.995f), Vec3(1.0f, 1.0f, 1.0f) } },
        { { Vec3(1.0f, 0.5f, 0.0f),   Quat(0.0f, 0.2f, 0.0f, 0.98f),  Vec3(1.0f, 1.0f, 1.0f) },
          { Vec3(0.0f, 0.75f, 0.25f), Quat(0.2f, 0.1f, 0.0f, 0.95f),  Vec3(1.0f, 0.5f, 1.0f) },
          { Vec3(0.5f, 0.0f, 0.0f),   Quat(0.0f, 0.0f, 0.2f, 0.98f),  Vec3(1.0f, 1.0f, 2.0f) } },
        { { Vec3(0.5f, -0.5f, 0.5f),  Quat(0.3f, 0.0f, 0.0f, 0.95f),  Vec3(2.0f, 1.0f, 1.0f) },
          { Vec3(-0.25f, 0.5f, 0.0f), Quat(0.0f, 0.0f, 0.3f, 0.95f),  Vec3(1.0f, 1.0f, 1.0f) },
          { Vec3(0.0f, 0.25f, -0.5f), Quat(0.1f, 0.2f, 0.1f, 0.95f),  Vec3(0.5f, 1.0f, 1.0f) } },
        { { Vec3(0.0f, 0.0f, 1.0f),   Quat(0.0f, 0.0f, 0.4f, 0.9f),   Vec3(1.0f, 2.0f, 1.0f) },
          { Vec3(0.5f, 0.0f, 0.5f),   Quat(-0.2f, 0.2f, 0.0f, 0.95f), Vec3(1.0f, 1.0f, 0.5f) },
          { Vec3(0.0f, -0.25f, 0.0f), Quat(0.25f, 0.0f, 0.1f, 0.95f), Vec3(1.0f, 1.5f, 1.0f) } },
    };
    rig.vertices = {
        { Vec3(0.1f, 0.2f, 0.3f),   Vec3(0.0f, 0.0f, 1.0f),  Vec4(1.0f, 0.0f, 0.0f, 1.0f),  { 0, 1, 0, 0 }, { 200, 55, 0, 0 } },
        { Vec3(-0.5f, 1.0f, 0.25f), Vec3(0.0f, 1.0f, 0.0f),  Vec4(0.0f, 0.0f, 1.0f, -1.0f), { 1, 2, 0, 0 }, { 128, 100, 27, 0 } },
        { Vec3(1.5f, -0.5f, 0.5f),  Vec3(1.0f, 0.0f, 0.0f),  Vec4(0.0f, 1.0f, 0.0f, 1.0f),  { 2, 0, 0, 0 }, { 255, 0, 0, 0 } },
        { Vec3(0.0f, 0.5f, -1.0f),  Vec3(0.6f, 0.8f, 0.0f),  Vec4(0.8f, -0.6f, 0.0f, -1.0f),{ 0, 1, 2, 0 }, { 85, 85, 85, 0 } },
        { Vec3(-1.0f, -1.0f, 2.0f), Vec3(0.0f, 0.7f, 0.7f),  Vec4(1.0f, 0.0f, 0.0f, 1.0f),  { 2, 1, 0, 0 }, { 60, 195, 0, 0 } },
    };
    return rig;
}

// A different two-joint rig: different bind pose, three frames, three verts.
inline Rig make_rig_beta()
{
    Rig rig;
    rig.joints = {
        { -1, Vec3(1.0f, 0.0f, -0.5f), Quat(0.2f, 0.1f, 0.0f, 0.97f), Vec3(1.5f, 1.0f, 1.0f) },
        {  0, Vec3(0.0f, 2.0f, 0.0f),  Quat(0.0f, 0.3f, 0.1f, 0.94f), Vec3(1.0f, 0.5f, 2.0f) },
    };
    rig.frames = {
        { { Vec3(0.0f, 0.0f, 0.0f),  Quat(0.0f, 0.0f, 0.0f, 1.0f),  Vec3(1.0f, 1.0f, 1.0f) },
          { Vec3(0.0f, 0.0f, 0.5f),  Quat(0.0f, 0.1f, 0.0f, 0.995f),Vec3(1.0f, 1.0f, 1.0f) } },
        { { Vec3(-1.0f, 0.25f, 0.0f),Quat(0.0f, 0.0f, 0.25f, 0.97f),Vec3(0.5f, 1.0f, 1.0f) },
          { Vec3(0.0f, -0.5f, 0.0f), Quat(0.15f, 0.0f, 0.0f, 0.99f),Vec3(1.0f, 1.0f, 1.0f) } },
        { { Vec3(0.5f, 0.0f, 0.75f), Quat(0.0f, -0.2f, 0.0f, 0.98f),Vec3(1.0f, 1.0f, 1.5f) },
          { Vec3(0.25f, 0.0f, 0.0f), Quat(0.0f, 0.0f, -0.2f, 0.98f),Vec3(1.0f, 2.0f, 1.0f) } },
    };
    rig.vertices = {
        { Vec3(0.25f, 0.0f, 0.5f),  Vec3(0.0f, 0.0f, 1.0f), Vec4(1.0f, 0.0f, 0.0f, 1.0f),  { 0, 1, 0, 0 }, { 180, 75, 0, 0 } },
        { Vec3(-0.75f, 0.5f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec4(0.0f, 0.0f, 1.0f, -1.0f), { 1, 0, 0, 0 }, { 255, 0, 0, 0 } },
        { Vec3(0.0f, -1.0f, 1.0f),  Vec3(1.0f, 0.0f, 0.0f), Vec4(0.0f, 1.0f, 0.0f, 1.0f),  { 0, 1, 0, 0 }, { 90, 165, 0, 0 } },
    };
    return rig;
}

} // namespace fixture

#endif
