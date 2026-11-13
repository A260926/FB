//****************************************************************************//
// transparentpacket.cpp                                                      //
// Geometry-splitting transparent draw packet with a reusable BSP tree.       //
//                                                                            //
// This library is free software; you can redistribute it and/or modify it    //
// under the terms of the GNU Lesser General Public License as published by   //
// the Free Software Foundation; either version 2.1 of the License, or (at    //
// your option) any later version.                                            //
//****************************************************************************//

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "cal3d/bonetransform.h"
#include "cal3d/color.h"
#include "cal3d/coremorphtarget.h"
#include "cal3d/coresubmesh.h"
#include "cal3d/physique.h"
#include "cal3d/quaternion.h"
#include "cal3d/submesh.h"
#include "cal3d/transform.h"
#include "cal3d/transparentpacket.h"
#include "cal3d/vector.h"
#include "cal3d/vector4.h"

namespace cal3d {
    TransparentBspNode::TransparentBspNode()
        : front(s_npos)
        , back(s_npos)
    {}

    namespace {
        const float kWeightTolerance = 1e-4f;
        const float kQuaternionTolerance = 1e-4f;

        bool isFinite(float f) {
            return std::isfinite(f);
        }

        bool isFinite(const CalVector& v) {
            return isFinite(v.x) && isFinite(v.y) && isFinite(v.z);
        }

        bool isFinite(const CalQuaternion& q) {
            return isFinite(q.x) && isFinite(q.y) && isFinite(q.z) && isFinite(q.w);
        }

        CalVector toVector(const CalVector4& v) {
            return CalVector(v.x, v.y, v.z);
        }

        TransparentCorner lerpCorner(
            const TransparentCorner& a,
            const TransparentCorner& b,
            float t
        ) {
            TransparentCorner c;
            c.position = a.position + (b.position - a.position) * t;
            c.normal = a.normal + (b.normal - a.normal) * t;
            c.u = a.u + (b.u - a.u) * t;
            c.v = a.v + (b.v - a.v) * t;
            c.r = a.r + (b.r - a.r) * t;
            c.g = a.g + (b.g - a.g) * t;
            c.b = a.b + (b.b - a.b) * t;
            c.a = a.a + (b.a - a.a) * t;
            for (int k = 0; k < 3; ++k) {
                // Barycentric weights stay relative to the original face:
                // every cut point is an affine combination of earlier cut
                // points, which themselves already point back to that face.
                c.barycentric[k] =
                    a.barycentric[k] + (b.barycentric[k] - a.barycentric[k]) * t;
            }
            return c;
        }

        struct Plane {
            CalVector normal; // unit length
            CalVector point;
        };

        float signedDistance(const Plane& plane, const TransparentCorner& c) {
            return dot(plane.normal, c.position - plane.point);
        }

        // 0 = on the plane (within epsilon), +1 / -1 = strict sides.
        int sideOf(float distance, float epsilon) {
            if (distance > epsilon) {
                return 1;
            }
            if (distance < -epsilon) {
                return -1;
            }
            return 0;
        }

        double crossLengthSquared(
            const CalVector& a,
            const CalVector& b,
            const CalVector& c
        ) {
            const CalVector ab = b - a;
            const CalVector ac = c - a;
            const CalVector cr = cross(ab, ac);
            return double(cr.x) * cr.x + double(cr.y) * cr.y + double(cr.z) * cr.z;
        }

        struct BuildContext {
            BuildContext(
                std::vector<TransparentFragment>& fragments,
                std::vector<TransparentBspNode>& nodes,
                std::vector<char>& live,
                float epsilon,
                size_t maxTriangles)
                : fragments(fragments)
                , nodes(nodes)
                , live(live)
                , epsilon(epsilon)
                , maxTriangles(maxTriangles)
                , liveCount(0)
                , failed(false)
            {}

            std::vector<TransparentFragment>& fragments;
            std::vector<TransparentBspNode>& nodes;
            std::vector<char>& live;
            const float epsilon;
            const size_t maxTriangles;
            size_t liveCount;
            bool failed;

            // The cap bounds the fragments that survive the build.  A split
            // replaces one live triangle with its pieces, so the parent is
            // retired first and only the net new pieces count.  Landing
            // exactly on the cap is allowed; the next addition would exceed
            // it and fails the whole build.
            bool canAdd() const {
                return liveCount < maxTriangles;
            }

            size_t addFragment(const TransparentFragment& fragment) {
                fragments.push_back(fragment);
                live.push_back(1);
                ++liveCount;
                return fragments.size() - 1;
            }

            void killFragment(size_t index) {
                if (live[index] != 0) {
                    live[index] = 0;
                    --liveCount;
                }
            }
        };

        // Clips one triangle against a plane.  Front pieces live in the closed
        // positive half-space, back pieces in the closed negative half-space.
        // On-plane corners go to both polygons so neither side loses coverage;
        // no solid caps are ever generated.
        void splitTriangle(
            const TransparentFragment& f,
            const Plane& plane,
            float epsilon,
            std::vector<TransparentFragment>& frontPieces,
            std::vector<TransparentFragment>& backPieces
        ) {
            std::vector<TransparentCorner> front;
            std::vector<TransparentCorner> back;

            float distance[3];
            int sides[3];
            for (int i = 0; i < 3; ++i) {
                distance[i] = signedDistance(plane, f.corner[i]);
                sides[i] = sideOf(distance[i], epsilon);
            }

            for (int i = 0; i < 3; ++i) {
                const int j = (i + 1) % 3;
                const TransparentCorner& a = f.corner[i];
                const TransparentCorner& b = f.corner[j];

                if (sides[i] >= 0) {
                    front.push_back(a);
                }
                if (sides[i] <= 0) {
                    back.push_back(a);
                }

                if ((sides[i] > 0 && sides[j] < 0)
                    || (sides[i] < 0 && sides[j] > 0)) {
                    const float t = distance[i] / (distance[i] - distance[j]);
                    const TransparentCorner cut = lerpCorner(a, b, t);
                    front.push_back(cut);
                    back.push_back(cut);
                }
            }

            struct Emit {
                static void run(
                    const std::vector<TransparentCorner>& polygon,
                    const TransparentFragment& source,
                    std::vector<TransparentFragment>& out
                ) {
                    if (polygon.size() < 3) {
                        return;
                    }
                    for (size_t k = 1; k + 1 < polygon.size(); ++k) {
                        // The fan follows the clipped polygon order, so the
                        // pieces keep the source face's winding.
                        if (crossLengthSquared(
                                polygon[0].position,
                                polygon[k].position,
                                polygon[k + 1].position) == 0.0) {
                            continue;
                        }
                        TransparentFragment piece;
                        piece.inputIndex = source.inputIndex;
                        piece.faceIndex = source.faceIndex;
                        piece.coreMaterialThreadId = source.coreMaterialThreadId;
                        piece.hasUV = source.hasUV;
                        piece.corner[0] = polygon[0];
                        piece.corner[1] = polygon[k];
                        piece.corner[2] = polygon[k + 1];
                        out.push_back(piece);
                    }
                }
            };

            Emit::run(front, f, frontPieces);
            Emit::run(back, f, backPieces);
        }

        // Validates one input and appends its world-space, morphed and skinned
        // triangles to the fragment pool.  Zero-area source faces are skipped.
        bool collectInput(
            const TransparentInput& input,
            size_t inputIndex,
            BuildContext& ctx
        ) {
            const CalSubmesh* submesh = input.submesh;
            if (submesh == 0 || submesh->coreSubmesh.get() == 0) {
                return false;
            }
            const CalCoreSubmesh* core = submesh->coreSubmesh.get();
            const size_t vertexCount = core->getVertexCount();
            const size_t faceCount = core->getFaceCount();
            if (vertexCount == 0 || faceCount == 0) {
                return false;
            }
            if (input.bones == 0 || input.boneCount == 0) {
                return false;
            }
            if (!isFinite(input.objectToWorld.rotation)
                || !isFinite(input.objectToWorld.translation)) {
                return false;
            }
            const CalQuaternion& q = input.objectToWorld.rotation;
            const float quaternionNorm2 =
                q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
            // Only rigid, unit-quaternion transforms are supported.
            if (std::fabs(quaternionNorm2 - 1.0f) > kQuaternionTolerance) {
                return false;
            }

            const CalCoreSubmesh::VectorVertex& coreVertices = core->getVectorVertex();
            const CalCoreSubmesh::InfluenceVector& influences = core->getInfluences();
            if (influences.empty()) {
                return false;
            }

            for (size_t v = 0; v < vertexCount; ++v) {
                if (!isFinite(toVector(coreVertices[v].position))
                    || !isFinite(toVector(coreVertices[v].normal))) {
                    return false;
                }
            }
            if (core->hasTextureCoordinates()) {
                const CalCoreSubmesh::VectorTextureCoordinate& uvs =
                    core->getTextureCoordinates();
                if (uvs.size() != vertexCount) {
                    return false;
                }
                for (size_t v = 0; v < vertexCount; ++v) {
                    if (!isFinite(uvs[v].u) || !isFinite(uvs[v].v)) {
                        return false;
                    }
                }
            }

            // Influences are stored flattened, one run per vertex, terminated
            // by the lastInfluenceForThisVertex flag.
            size_t influenceCursor = 0;
            for (size_t v = 0; v < vertexCount; ++v) {
                if (influenceCursor >= influences.size()) {
                    return false;
                }
                float weightSum = 0.0f;
                bool terminated = false;
                while (influenceCursor < influences.size()) {
                    const CalCoreSubmesh::Influence& inf = influences[influenceCursor];
                    if (!isFinite(inf.weight) || inf.weight < 0.0f) {
                        return false;
                    }
                    if (inf.boneId >= input.boneCount) {
                        return false;
                    }
                    weightSum += inf.weight;
                    const bool last = inf.lastInfluenceForThisVertex != 0;
                    ++influenceCursor;
                    if (last) {
                        terminated = true;
                        break;
                    }
                }
                if (!terminated
                    || std::fabs(weightSum - 1.0f) > kWeightTolerance) {
                    return false;
                }
            }
            if (influenceCursor != influences.size()) {
                return false;
            }

            for (size_t m = 0; m < submesh->morphTargets.size(); ++m) {
                if (!isFinite(submesh->morphTargets[m].weight)) {
                    return false;
                }
            }

            const CalCoreSubmesh::VectorFace& faces = core->getFaces();
            for (size_t f = 0; f < faceCount; ++f) {
                for (int k = 0; k < 3; ++k) {
                    if (faces[f].vertexId[k] >= vertexCount) {
                        return false;
                    }
                }
            }
            for (size_t m = 0; m < core->getMorphTargets().size(); ++m) {
                const CalCoreMorphTarget& target = *core->getMorphTargets()[m];
                for (size_t k = 0; k < target.vertexOffsets.size(); ++k) {
                    if (target.vertexOffsets[k].vertexId >= vertexCount) {
                        return false;
                    }
                }
            }

            // Go through CalPhysique's own morph-accumulation plus skinning
            // path on a private buffer.  The routine reads the submesh's
            // morph weights and bone palette without modifying either one.
            std::vector<CalVector4> skinBuffer(vertexCount * 2);
            CalPhysique::calculateVerticesAndNormals(
                input.bones,
                submesh,
                &skinBuffer[0].x);

            for (size_t k = 0; k < skinBuffer.size(); ++k) {
                if (!isFinite(toVector(skinBuffer[k]))) {
                    return false;
                }
            }

            const RotateTranslate& objectToWorld = input.objectToWorld;
            std::vector<CalVector> worldPositions(vertexCount);
            std::vector<CalVector> worldNormals(vertexCount);
            for (size_t v = 0; v < vertexCount; ++v) {
                const CalVector localPosition = toVector(skinBuffer[v * 2]);
                const CalVector localNormal = toVector(skinBuffer[v * 2 + 1]);
                worldPositions[v] =
                    objectToWorld.rotation * localPosition
                    + objectToWorld.translation;
                // Normals follow the rigid rotation only and stay
                // unnormalized, matching the skinning output.
                worldNormals[v] = objectToWorld.rotation * localNormal;
                if (!isFinite(worldPositions[v]) || !isFinite(worldNormals[v])) {
                    return false;
                }
            }

            const bool hasUV = core->hasTextureCoordinates();
            const CalCoreSubmesh::VectorTextureCoordinate& uvs =
                core->getTextureCoordinates();
            const std::vector<CalColor32>& colors = core->getVertexColors();

            for (size_t f = 0; f < faceCount; ++f) {
                const CalIndex* id = faces[f].vertexId;
                const CalVector& p0 = worldPositions[id[0]];
                const CalVector& p1 = worldPositions[id[1]];
                const CalVector& p2 = worldPositions[id[2]];
                if (crossLengthSquared(p0, p1, p2) == 0.0) {
                    continue; // zero-area source face: ignored, not an error
                }

                if (!ctx.canAdd()) {
                    return false;
                }

                TransparentFragment fragment;
                fragment.inputIndex = inputIndex;
                fragment.faceIndex = f;
                fragment.coreMaterialThreadId = core->coreMaterialThreadId;
                fragment.hasUV = hasUV;

                for (int corner = 0; corner < 3; ++corner) {
                    const CalIndex vertexId = id[corner];
                    TransparentCorner& dst = fragment.corner[corner];
                    dst.position = worldPositions[vertexId];
                    dst.normal = worldNormals[vertexId];
                    if (hasUV) {
                        dst.u = uvs[vertexId].u;
                        dst.v = uvs[vertexId].v;
                    } else {
                        dst.u = 0.0f;
                        dst.v = 0.0f;
                    }
                    // Unpack to float RGBA before any interpolation happens.
                    const CalVector4 rgba = CalVector4FromColor(colors[vertexId]);
                    dst.r = rgba.x;
                    dst.g = rgba.y;
                    dst.b = rgba.z;
                    dst.a = rgba.w;
                    dst.barycentric[0] = corner == 0 ? 1.0f : 0.0f;
                    dst.barycentric[1] = corner == 1 ? 1.0f : 0.0f;
                    dst.barycentric[2] = corner == 2 ? 1.0f : 0.0f;
                }
                ctx.addFragment(fragment);
            }

            return true;
        }

        bool makePlane(const TransparentFragment& f, Plane& plane) {
            const CalVector e1 = f.corner[1].position - f.corner[0].position;
            const CalVector e2 = f.corner[2].position - f.corner[0].position;
            CalVector normal = cross(e1, e2);
            const float length = normal.length();
            if (length == 0.0f || !isFinite(length)) {
                return false;
            }
            normal /= length;
            if (!isFinite(normal)) {
                return false;
            }
            plane.normal = normal;
            plane.point = f.corner[0].position;
            return true;
        }

        struct PendingNode {
            std::vector<size_t> items;
            size_t parent;
            bool attachFront;
        };

        // Builds one BSP over every instance's triangles together.  Splitter
        // planes always come from a real triangle produced by this build.
        bool buildBsp(BuildContext& ctx, size_t& root) {
            root = TransparentBspNode::s_npos;

            std::vector<PendingNode> stack;
            stack.push_back(PendingNode());
            for (size_t i = 0; i < ctx.fragments.size(); ++i) {
                stack.back().items.push_back(i);
            }
            stack.back().parent = TransparentBspNode::s_npos;
            stack.back().attachFront = false;

            while (!stack.empty()) {
                PendingNode job;
                job.items.swap(stack.back().items);
                job.parent = stack.back().parent;
                job.attachFront = stack.back().attachFront;
                stack.pop_back();

                if (job.items.empty()) {
                    continue;
                }

                Plane plane;
                if (!makePlane(ctx.fragments[job.items[0]], plane)) {
                    return false;
                }

                TransparentBspNode node;
                node.planeNormal = plane.normal;
                node.planePoint = plane.point;
                const size_t nodeIndex = ctx.nodes.size();
                ctx.nodes.push_back(node);
                if (job.parent == TransparentBspNode::s_npos) {
                    root = nodeIndex;
                } else if (job.attachFront) {
                    ctx.nodes[job.parent].front = nodeIndex;
                } else {
                    ctx.nodes[job.parent].back = nodeIndex;
                }

                std::vector<size_t> frontItems;
                std::vector<size_t> backItems;

                for (size_t item : job.items) {
                    const TransparentFragment& fragment = ctx.fragments[item];
                    int corners[3];
                    bool hasFront = false;
                    bool hasBack = false;
                    for (int k = 0; k < 3; ++k) {
                        const float d = signedDistance(plane, fragment.corner[k]);
                        corners[k] = sideOf(d, ctx.epsilon);
                        hasFront = hasFront || corners[k] > 0;
                        hasBack = hasBack || corners[k] < 0;
                    }

                    if (!hasFront && !hasBack) {
                        ctx.nodes[nodeIndex].items.push_back(item);
                    } else if (!hasBack) {
                        frontItems.push_back(item);
                    } else if (!hasFront) {
                        backItems.push_back(item);
                    } else {
                        std::vector<TransparentFragment> frontPieces;
                        std::vector<TransparentFragment> backPieces;
                        splitTriangle(
                            fragment,
                            plane,
                            ctx.epsilon,
                            frontPieces,
                            backPieces);

                        // The parent is replaced by its pieces: retire it,
                        // then make sure the net growth stays within the cap.
                        const size_t pieceCount =
                            frontPieces.size() + backPieces.size();
                        if (pieceCount == 0
                            || ctx.liveCount + pieceCount - 1 > ctx.maxTriangles) {
                            ctx.failed = true;
                            return false;
                        }
                        ctx.killFragment(item);
                        for (const TransparentFragment& piece : frontPieces) {
                            frontItems.push_back(ctx.addFragment(piece));
                        }
                        for (const TransparentFragment& piece : backPieces) {
                            backItems.push_back(ctx.addFragment(piece));
                        }
                    }
                }

                PendingNode frontJob;
                frontJob.items.swap(frontItems);
                frontJob.parent = nodeIndex;
                frontJob.attachFront = true;
                stack.push_back(std::move(frontJob));

                PendingNode backJob;
                backJob.items.swap(backItems);
                backJob.parent = nodeIndex;
                backJob.attachFront = false;
                stack.push_back(std::move(backJob));
            }

            return true;
        }
    }

    TransparentPacket::TransparentPacket()
        : m_root(TransparentBspNode::s_npos)
    {}

    bool TransparentPacket::build(
        const TransparentInput* inputs,
        size_t inputCount,
        float epsilon,
        size_t maxTriangles)
    {
        if (!isFinite(epsilon) || epsilon <= 0.0f) {
            return false;
        }
        if (maxTriangles == 0) {
            return false;
        }
        if (inputCount > 0 && inputs == 0) {
            return false;
        }

        // Everything lands in local storage first; only a fully successful
        // build replaces the previous packet, so a failure leaves the last
        // good packet (and any fragments the caller copied out of it) alone.
        std::vector<TransparentFragment> newFragments;
        std::vector<TransparentBspNode> newNodes;
        std::vector<char> live;
        BuildContext ctx(
            newFragments, newNodes, live, epsilon, maxTriangles);

        for (size_t i = 0; i < inputCount; ++i) {
            if (!collectInput(inputs[i], i, ctx)) {
                return false;
            }
            if (ctx.failed) {
                return false;
            }
        }

        size_t newRoot = TransparentBspNode::s_npos;
        if (!newFragments.empty()) {
            if (!buildBsp(ctx, newRoot)) {
                return false;
            }
        }
        if (ctx.failed) {
            return false;
        }

        // Split-away parents are dead: compact the pool and remap every node.
        std::vector<size_t> oldToNew(newFragments.size(), TransparentBspNode::s_npos);
        std::vector<TransparentFragment> compacted;
        compacted.reserve(newFragments.size());
        for (size_t i = 0; i < newFragments.size(); ++i) {
            if (!live[i]) {
                continue;
            }
            oldToNew[i] = compacted.size();
            compacted.push_back(newFragments[i]);
        }
        if (compacted.size() > maxTriangles) {
            return false;
        }
        for (TransparentBspNode& node : newNodes) {
            for (size_t& item : node.items) {
                item = oldToNew[item];
            }
            // Coplanar fragments draw in submission order.
            std::sort(node.items.begin(), node.items.end(),
                [&compacted](size_t lhs, size_t rhs) {
                    const TransparentFragment& a = compacted[lhs];
                    const TransparentFragment& b = compacted[rhs];
                    if (a.inputIndex != b.inputIndex) {
                        return a.inputIndex < b.inputIndex;
                    }
                    if (a.faceIndex != b.faceIndex) {
                        return a.faceIndex < b.faceIndex;
                    }
                    return lhs < rhs;
                });
        }

        m_fragments.swap(compacted);
        m_nodes.swap(newNodes);
        m_root = newRoot;
        return true;
    }

    bool TransparentPacket::order(
        const CalVector& eye,
        std::vector<size_t>& out) const
    {
        if (!isFinite(eye)) {
            return false;
        }

        std::vector<size_t> result;
        result.reserve(m_fragments.size());

        struct Job {
            size_t node;
            bool emitItems;
        };
        std::vector<Job> stack;
        if (m_root != TransparentBspNode::s_npos) {
            stack.push_back(Job{m_root, false});
        }

        while (!stack.empty()) {
            const Job job = stack.back();
            stack.pop_back();
            if (job.node == TransparentBspNode::s_npos) {
                continue;
            }
            const TransparentBspNode& node = m_nodes[job.node];

            if (job.emitItems) {
                result.insert(
                    result.end(), node.items.begin(), node.items.end());
                continue;
            }

            const float distance = dot(node.planeNormal, eye - node.planePoint);
            // An eye exactly on the plane walks the negative-side subtree
            // first, so the zero-distance case uses the >= branch.
            size_t firstChild;   // drawn first (farther from the eye)
            size_t secondChild;  // drawn last (nearer the eye)
            if (distance >= 0.0f) {
                firstChild = node.back;
                secondChild = node.front;
            } else {
                firstChild = node.front;
                secondChild = node.back;
            }

            // LIFO stack: second child is pushed first so it emits last.
            stack.push_back(Job{secondChild, false});
            stack.push_back(Job{job.node, true});
            stack.push_back(Job{firstChild, false});
        }

        out.swap(result);
        return true;
    }
}
