//****************************************************************************//
// transparentpacket.h                                                        //
// Geometry-splitting transparent draw packet with a reusable BSP tree.       //
//                                                                            //
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#pragma once

#include <cstddef>
#include <vector>

#include "cal3d/global.h"
#include "cal3d/transform.h"
#include "cal3d/vector.h"

class CalSubmesh;
struct BoneTransform;

namespace cal3d {

    // One corner of a split fragment.  Every attribute is expressed in world
    // space and every corner remembers its barycentric coordinates relative to
    // the three corners of the original, unsplit face.
    struct TransparentCorner {
        CalVector position = CalVector(0.0f, 0.0f, 0.0f); // world space position
        CalVector normal = CalVector(0.0f, 0.0f, 0.0f);   // world space normal, not normalized
        float u = 0.0f;                     // texture coordinate (0 when the fragment has no UVs)
        float v = 0.0f;
        float r = 0.0f;                     // float RGBA, unpacked before interpolation
        float g = 0.0f;
        float b = 0.0f;
        float a = 0.0f;
        float barycentric[3] = {0.0f, 0.0f, 0.0f}; // weights relative to the original face corners
    };

    // A single drawable triangle produced by the BSP split.  Winding, coverage
    // and area relative to the original face are preserved.
    struct TransparentFragment {
        size_t inputIndex;           // index of the Input this fragment came from
        size_t faceIndex;            // index of the face inside its source submesh
        int coreMaterialThreadId;
        bool hasUV;
        TransparentCorner corner[3];
    };

    // One skinned mesh instance participating in the same transparent packet.
    // The bone palette already holds the current pose; objectToWorld is a rigid
    // transform whose quaternion must be unit length.
    struct TransparentInput {
        const CalSubmesh* submesh;
        const BoneTransform* bones;
        size_t boneCount;
        RotateTranslate objectToWorld;
    };

    // Internal BSP node.  Exposed only because the packet stores a vector of
    // them; callers should treat this as an implementation detail.
    struct TransparentBspNode {
        TransparentBspNode();

        CalVector planeNormal;    // unit length, taken from a real triangle
        CalVector planePoint;
        size_t front;             // node on the positive side, s_npos when empty
        size_t back;              // node on the negative side, s_npos when empty
        std::vector<size_t> items; // fragment indices coplanar with this node

        static const size_t s_npos = static_cast<size_t>(-1);
    };

    // Build once per frame/pose, then ask for back-to-front orders from any
    // number of cameras.  A successful build owns all of its geometry: the
    // source submeshes and palettes may be released afterwards.
    class CAL3D_API TransparentPacket {
    public:
        TransparentPacket();

        // Runs morph + skinning through CalPhysique, moves every instance into
        // world space, and inserts all triangles into one shared BSP tree,
        // physically splitting crossing polygons.  On failure the previously
        // built packet is left untouched.  An empty input set is a legal empty
        // scene.  Returns true on success.
        bool build(
            const TransparentInput* inputs,
            size_t inputCount,
            float epsilon,
            size_t maxTriangles);

        // Writes a permutation of fragments() indices such that drawing the
        // fragments in that order with plain source-over blending is
        // back-to-front as seen from the world-space eye.  Only reads the
        // built tree.  Returns false (leaving out untouched) on a non-finite
        // eye.
        bool order(const CalVector& eye, std::vector<size_t>& out) const;

        const std::vector<TransparentFragment>& fragments() const {
            return m_fragments;
        }

    private:
        std::vector<TransparentFragment> m_fragments;
        std::vector<TransparentBspNode> m_nodes;
        size_t m_root;
    };
}
