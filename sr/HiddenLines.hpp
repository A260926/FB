//
//  SoftwareRenderer
//

#ifndef SR_HIDDEN_LINES_HPP
#define SR_HIDDEN_LINES_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>
#include "PreparedMesh.hpp"
#include "Rect.hpp"
#include "Vector.hpp"

namespace sr
{
    // One visible interval of an original mesh edge, in viewport
    // coordinates. edgeIndex refers to PreparedMesh::getEdges(); index0 and
    // index1 are the original vertex indices of the edge (index0 at
    // t = 0, index1 at t = 1). [t0, t1] is the linear parameter interval on
    // the original edge, p0/p1 the matching viewport-space endpoints.
    struct VisibleEdgeSegment final
    {
        std::size_t edgeIndex = 0;
        std::size_t index0 = 0;
        std::size_t index1 = 0;
        double t0 = 0.0;
        double t1 = 0.0;
        Vector<float, 2> p0;
        Vector<float, 2> p1;
    };

    // Continuous hidden-line removal for a prepared mesh.
    //
    // Candidate edges are boundary edges, silhouette edges (one adjacent
    // face front-facing, the other back-facing) and crease edges (angle
    // between the outward normals of the two adjacent faces greater than
    // creaseAngle, in radians). Coplanar internal diagonals are never
    // emitted. Only original edges are clipped against the frustum; no
    // frustum cut lines or fan-internal edges are produced.
    //
    // viewPositions holds one view-space position per original vertex (the
    // eye is at the origin); it is used for face orientation and dihedral
    // angles. Occlusion is computed analytically in the continuous screen
    // domain against all clipped faces, treated as double-sided and
    // opaque: an edge point is hidden exactly where a face covers it with
    // strictly smaller depth (equal depth does not occlude, and the faces
    // adjacent to the edge never erase it). The occluding intervals are
    // united and subtracted from the clipped edge; the remaining visible
    // intervals are reported with the original edge parameter, even though
    // the screen-space parameterization differs from it whenever the two
    // endpoints have different w.
    //
    // The viewport may have a non-zero origin and a non-square size; the
    // screen y direction matches the rest of the library. The result is
    // sorted by edge index, and intervals that touch on the same edge are
    // merged. Only segments with a positive screen length are emitted.
    //
    // Throws std::invalid_argument on non-finite view positions, a view
    // position count mismatch, a non-finite crease angle or an invalid
    // viewport (non-finite or non-positive size).
    [[nodiscard]] inline std::vector<VisibleEdgeSegment> visibleEdges(
        const PreparedMesh& mesh,
        const std::vector<Vector<float, 3>>& viewPositions,
        const Rect<float>& viewport,
        const float creaseAngle)
    {
        if (!std::isfinite(viewport.position.v[0]) || !std::isfinite(viewport.position.v[1]) ||
            !std::isfinite(viewport.size.v[0]) || !std::isfinite(viewport.size.v[1]) ||
            viewport.size.v[0] <= 0.0F || viewport.size.v[1] <= 0.0F)
            throw std::invalid_argument{"Invalid viewport"};

        if (!std::isfinite(creaseAngle))
            throw std::invalid_argument{"Invalid crease angle"};

        if (viewPositions.size() != mesh.getVertexCount())
            throw std::invalid_argument{"View position count must match the vertex count"};

        for (const auto& position : viewPositions)
            for (const float c : position.v)
                if (!std::isfinite(c))
                    throw std::invalid_argument{"View positions must be finite"};

        const auto& indices = mesh.getIndices();
        const auto& vertexOutputs = mesh.getVertexOutputs();
        const auto& triangles = mesh.getPreciseTriangles();
        const auto& edges = mesh.getEdges();

        const auto planeDistance = [](const int plane, const double p[4]) noexcept
        {
            switch (plane)
            {
                case 0: return p[0] + p[3]; // x >= -w
                case 1: return p[3] - p[0]; // x <= w
                case 2: return p[1] + p[3]; // y >= -w
                case 3: return p[3] - p[1]; // y <= w
                case 4: return p[2];        // z >= 0
                default: return p[3] - p[2]; // z <= w
            }
        };

        // outward normals and orientation of the original faces in view
        // space (eye at the origin): a face is front-facing when its
        // outward normal points towards the eye
        struct FaceInfo final
        {
            double nx = 0.0, ny = 0.0, nz = 0.0;
            bool front = false;
        };
        std::vector<FaceInfo> faces(indices.size() / 3);
        for (std::size_t f = 0; f < faces.size(); ++f)
        {
            const double p0[3]{viewPositions[indices[f * 3 + 0]].v[0],
                               viewPositions[indices[f * 3 + 0]].v[1],
                               viewPositions[indices[f * 3 + 0]].v[2]};
            const double p1[3]{viewPositions[indices[f * 3 + 1]].v[0],
                               viewPositions[indices[f * 3 + 1]].v[1],
                               viewPositions[indices[f * 3 + 1]].v[2]};
            const double p2[3]{viewPositions[indices[f * 3 + 2]].v[0],
                               viewPositions[indices[f * 3 + 2]].v[1],
                               viewPositions[indices[f * 3 + 2]].v[2]};
            const double ux = p1[0] - p0[0], uy = p1[1] - p0[1], uz = p1[2] - p0[2];
            const double vx = p2[0] - p0[0], vy = p2[1] - p0[1], vz = p2[2] - p0[2];
            FaceInfo& info = faces[f];
            info.nx = uy * vz - uz * vy;
            info.ny = uz * vx - ux * vz;
            info.nz = ux * vy - uy * vx;
            info.front = info.nx * p0[0] + info.ny * p0[1] + info.nz * p0[2] < 0.0;
        }

        const double viewportX = viewport.position.v[0];
        const double viewportY = viewport.position.v[1];
        const double viewportW = viewport.size.v[0];
        const double viewportH = viewport.size.v[1];
        const auto toViewport = [&](const double ndcX, const double ndcY,
                                    double& screenX, double& screenY) noexcept
        {
            screenX = ndcX * viewportW / 2.0 + viewportX + viewportW / 2.0;
            screenY = ndcY * viewportH / 2.0 + viewportY + viewportH / 2.0;
        };

        std::vector<VisibleEdgeSegment> result;
        std::vector<std::pair<double, double>> occluded;

        for (std::size_t e = 0; e < edges.size(); ++e)
        {
            const PreparedMesh::Edge& edge = edges[e];

            bool candidate = false;
            if (edge.face1 == PreparedMesh::npos)
                candidate = true; // boundary edge
            else
            {
                const FaceInfo& f0 = faces[edge.face0];
                const FaceInfo& f1 = faces[edge.face1];
                if (f0.front != f1.front)
                    candidate = true; // silhouette edge
                else
                {
                    // crease edge: angle between the outward normals
                    const double cx = f0.ny * f1.nz - f0.nz * f1.ny;
                    const double cy = f0.nz * f1.nx - f0.nx * f1.nz;
                    const double cz = f0.nx * f1.ny - f0.ny * f1.nx;
                    const double angle = std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz),
                                                    f0.nx * f1.nx + f0.ny * f1.ny + f0.nz * f1.nz);
                    if (angle > static_cast<double>(creaseAngle))
                        candidate = true;
                }
            }
            if (!candidate)
                continue;

            // the original edge in clip space
            const double a[4]{vertexOutputs[edge.v0].position.v[0],
                              vertexOutputs[edge.v0].position.v[1],
                              vertexOutputs[edge.v0].position.v[2],
                              vertexOutputs[edge.v0].position.v[3]};
            const double b[4]{vertexOutputs[edge.v1].position.v[0],
                              vertexOutputs[edge.v1].position.v[1],
                              vertexOutputs[edge.v1].position.v[2],
                              vertexOutputs[edge.v1].position.v[3]};

            // clip the original edge against the frustum
            double t0 = 0.0, t1 = 1.0;
            bool outside = false;
            for (int plane = 0; plane < 6 && !outside; ++plane)
            {
                const double d0 = planeDistance(plane, a);
                const double d1 = planeDistance(plane, b);
                if (d0 < 0.0 && d1 < 0.0)
                    outside = true;
                else if (d0 < 0.0)
                    t0 = std::max(t0, d0 / (d0 - d1));
                else if (d1 < 0.0)
                    t1 = std::min(t1, d0 / (d0 - d1));
            }
            if (outside || !(t1 > t0))
                continue;

            double p0[4], p1[4];
            for (int k = 0; k < 4; ++k)
            {
                p0[k] = a[k] + t0 * (b[k] - a[k]);
                p1[k] = a[k] + t1 * (b[k] - a[k]);
            }
            const double w0 = p0[3];
            const double w1 = p1[3];
            if (w0 <= 0.0 || w1 <= 0.0)
                continue; // the edge passes through the eye

            double sx0, sy0, sx1, sy1;
            toViewport(p0[0] / w0, p0[1] / w0, sx0, sy0);
            toViewport(p1[0] / w1, p1[1] / w1, sx1, sy1);
            const double depth0 = p0[2] / w0;
            const double depth1 = p1[2] / w1;
            const double edgeX = sx1 - sx0;
            const double edgeY = sy1 - sy0;
            if (edgeX == 0.0 && edgeY == 0.0)
                continue; // the edge projects to a point

            // Occlusion: intervals of the edge covered by strictly closer
            // faces, in the continuous screen domain. u in [0, 1]
            // parameterizes the projected (clipped) segment.
            occluded.clear();
            for (const PreparedMesh::PreciseClippedTriangle& triangle : triangles)
            {
                if (triangle.faceIndex == edge.face0 || triangle.faceIndex == edge.face1)
                    continue; // adjacent faces never erase their own edge

                double tx[3], ty[3], tz[3];
                bool projectable = true;
                for (int k = 0; k < 3; ++k)
                {
                    const double cw = triangle.positions[k][3];
                    if (cw <= 0.0)
                    {
                        projectable = false;
                        break;
                    }
                    toViewport(triangle.positions[k][0] / cw,
                               triangle.positions[k][1] / cw,
                               tx[k], ty[k]);
                    tz[k] = triangle.positions[k][2] / cw;
                }
                if (!projectable)
                    continue;

                const double e0x = tx[1] - tx[0], e0y = ty[1] - ty[0];
                const double e1x = tx[2] - tx[0], e1y = ty[2] - ty[0];
                const double area2 = e0x * e1y - e1x * e0y;
                if (area2 == 0.0)
                    continue; // zero-area projection cannot cover an interval

                // overlap of the projected edge with the projected triangle
                double ua = 0.0, ub = 1.0;
                for (int k = 0; k < 3 && ua < ub; ++k)
                {
                    const int k2 = (k + 1) % 3;
                    const double gx = tx[k2] - tx[k];
                    const double gy = ty[k2] - ty[k];
                    double fa = gx * (sy0 - ty[k]) - gy * (sx0 - tx[k]);
                    double fb = gx * (sy1 - ty[k]) - gy * (sx1 - tx[k]);
                    if (area2 < 0.0)
                    {
                        fa = -fa;
                        fb = -fb;
                    }
                    const double df = fb - fa;
                    if (df > 0.0)
                        ua = std::max(ua, -fa / df);
                    else if (df < 0.0)
                        ub = std::min(ub, -fa / df);
                    else if (fa < 0.0)
                    {
                        ua = 1.0;
                        ub = 0.0;
                    }
                }
                if (!(ua < ub))
                    continue;

                // face depth is affine in screen space, edge depth is
                // affine in u, so their difference has at most one root
                const double alpha = ((tz[1] - tz[0]) * e1y - (tz[2] - tz[0]) * e0y) / area2;
                const double beta = (e0x * (tz[2] - tz[0]) - e1x * (tz[1] - tz[0])) / area2;
                const auto depthDelta = [&](const double u) noexcept
                {
                    const double px = sx0 + u * edgeX;
                    const double py = sy0 + u * edgeY;
                    return (depth0 + u * (depth1 - depth0)) -
                        (tz[0] + alpha * (px - tx[0]) + beta * (py - ty[0]));
                };
                const double da = depthDelta(ua);
                const double db = depthDelta(ub);
                if (da <= 0.0 && db <= 0.0)
                    continue; // the face is never strictly closer

                double l0 = 0.0, l1 = 1.0;
                const double slope = db - da;
                if (slope > 0.0)
                    l0 = -da / slope;
                else if (slope < 0.0)
                    l1 = -da / slope;
                l0 = std::clamp(l0, 0.0, 1.0);
                l1 = std::clamp(l1, 0.0, 1.0);
                if (!(l1 > l0))
                    continue;

                const double uLo = ua + l0 * (ub - ua);
                const double uHi = ua + l1 * (ub - ua);

                // map the screen-space parameter back to the original edge
                // parameter (they differ when the endpoint w's differ)
                const auto toEdgeParameter = [&](const double u) noexcept
                {
                    const double tau = (u * w0) / ((1.0 - u) * w1 + u * w0);
                    return t0 + (t1 - t0) * tau;
                };
                occluded.emplace_back(toEdgeParameter(uLo), toEdgeParameter(uHi));
            }

            // unite the occluded intervals; gaps at the level of floating
            // point noise (independently clipped faces meet along their
            // shared edges, and an edge endpoint may touch an occluder)
            // do not break the union
            std::sort(occluded.begin(), occluded.end());
            const double unionTolerance = 1.0E-9 * std::max(1.0, t1 - t0);
            std::vector<std::pair<double, double>> united;
            for (const auto& interval : occluded)
            {
                double lo = std::max(interval.first, t0);
                double hi = std::min(interval.second, t1);
                if (lo - t0 <= unionTolerance)
                    lo = t0;
                if (t1 - hi <= unionTolerance)
                    hi = t1;
                if (!(hi > lo))
                    continue;
                if (!united.empty() && lo <= united.back().second + unionTolerance)
                    united.back().second = std::max(united.back().second, hi);
                else
                    united.emplace_back(lo, hi);
            }

            // complement gives the visible intervals
            std::vector<std::pair<double, double>> visible;
            double current = t0;
            for (const auto& interval : united)
            {
                if (interval.first > current)
                    visible.emplace_back(current, interval.first);
                if (interval.second > current)
                    current = interval.second;
            }
            if (t1 > current)
                visible.emplace_back(current, t1);

            // merge intervals that touch on the same edge
            std::vector<std::pair<double, double>> merged;
            for (const auto& interval : visible)
            {
                if (!merged.empty() && interval.first <= merged.back().second)
                    merged.back().second = interval.second;
                else
                    merged.push_back(interval);
            }

            for (const auto& interval : merged)
            {
                if (!(interval.second > interval.first))
                    continue;

                double qa[4], qb[4];
                for (int k = 0; k < 4; ++k)
                {
                    qa[k] = a[k] + interval.first * (b[k] - a[k]);
                    qb[k] = a[k] + interval.second * (b[k] - a[k]);
                }
                if (qa[3] <= 0.0 || qb[3] <= 0.0)
                    continue;

                double ax, ay, bx, by;
                toViewport(qa[0] / qa[3], qa[1] / qa[3], ax, ay);
                toViewport(qb[0] / qb[3], qb[1] / qb[3], bx, by);
                const double dx = bx - ax;
                const double dy = by - ay;
                if (dx * dx + dy * dy <= 0.0)
                    continue; // only positive screen length is emitted

                VisibleEdgeSegment segment;
                segment.edgeIndex = e;
                segment.index0 = edge.v0;
                segment.index1 = edge.v1;
                segment.t0 = interval.first;
                segment.t1 = interval.second;
                segment.p0 = Vector<float, 2>{static_cast<float>(ax), static_cast<float>(ay)};
                segment.p1 = Vector<float, 2>{static_cast<float>(bx), static_cast<float>(by)};
                result.push_back(segment);
            }
        }

        return result;
    }
}

#endif
