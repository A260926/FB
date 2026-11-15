/**
 * PatchEditor demonstration.
 *
 * Builds three rational patches that meet along two whole edges (a seam
 * chain), performs two drags, rejects one conflicting drag, and writes the
 * mesh of the last successful solve to the requested OBJ path.
 *
 * Usage: patch_editor <output.obj>
 *
 * Use of this source code is governed by a BSD-style license that can be found in
 * the LICENSE file.
 */

#include <tinynurbs/tinynurbs.h>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

using tinynurbs::PatchEditor;
using tinynurbs::PatchHandle;
using tinynurbs::PatchMesh;
using tinynurbs::PatchSeam;
using tinynurbs::RationalSurface3d;
using tinynurbs::SurfaceEdge;

// A non-planar rational patch over the given parameter rectangle.
RationalSurface3d makePatch(unsigned degree, double ua, double ub, double va, double vb,
                            double z_base)
{
    RationalSurface3d srf;
    srf.degree_u = degree;
    srf.degree_v = degree;
    srf.knots_u = {ua, ua, ua, ub, ub, ub};
    srf.knots_v = {va, va, va, vb, vb, vb};
    srf.control_points.resize(3, 3);
    srf.weights.resize(3, 3);
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
        {
            const double x = i * 0.9;
            const double y = j * 0.9;
            srf.control_points(i, j) =
                glm::dvec3(x, y, z_base + 0.35 * std::sin(1.1 * x + 0.4 * y) + 0.08 * x * y);
            srf.weights(i, j) = 0.75 + 0.2 * std::cos(0.7 * i + 1.3 * j);
        }
    }
    return srf;
}

int fail(const char *message)
{
    std::fprintf(stderr, "patch_editor example: %s\n", message);
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        return fail("expected exactly one argument: <output.obj>");
    }
    const std::string out_path = argv[1];

    // Three patches laid out left -> middle -> right, each in its own
    // (translated) parameter rectangle.
    std::vector<RationalSurface3d> patches;
    patches.push_back(makePatch(2, -2.0, 0.0, 3.0, 5.0, 0.0));
    patches.push_back(makePatch(2, 10.0, 12.0, -1.0, 1.0, 0.0));
    patches.push_back(makePatch(2, 0.0, 2.0, 20.0, 22.0, 0.0));

    std::vector<PatchSeam> seams;
    seams.push_back({0, SurfaceEdge::UMax, 1, SurfaceEdge::UMin, false});
    seams.push_back({1, SurfaceEdge::UMax, 2, SurfaceEdge::UMin, false});

    PatchEditor editor(patches, seams);

    // Before any drag: write the rest-state mesh next to the requested file so
    // the before/after difference can be inspected.
    tinynurbs::PatchEditorSnapshot rest = editor.solve();
    {
        std::string before_path = out_path;
        const std::size_t dot = before_path.find_last_of('.');
        const std::size_t slash = before_path.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        {
            before_path = before_path.substr(0, dot) + ".before" + before_path.substr(dot);
        }
        else
        {
            before_path += ".before.obj";
        }
        if (!tinynurbs::writePatchMeshOBJ(tinynurbs::buildPatchMesh(rest), before_path))
        {
            return fail("could not write the pre-drag OBJ file");
        }
    }

    // Drag 1: pull a point of the middle patch.
    PatchHandle handle;
    handle.id = 100;
    handle.patch = 1;
    handle.u = 11.0;
    handle.v = 0.0;
    handle.target = glm::dvec3(3.5, 1.0, 2.2);
    editor.set_handles({handle});
    editor.solve();
    const std::size_t factorizations_after_first = editor.factorization_count();

    // Drag 2: same handles, new target.  Reuses the factorization.
    handle.target = glm::dvec3(4.0, 1.5, 2.7);
    editor.set_handles({handle});
    auto snapshot = editor.solve();
    if (editor.factorization_count() != factorizations_after_first)
    {
        return fail("re-targeting the same handles must reuse the factorization");
    }

    // Conflicting drag: two handles on the same surface point with different
    // targets.  set_handles must reject it and leave the staged list intact.
    PatchHandle conflict = handle;
    conflict.id = 200;
    conflict.target = glm::dvec3(-9.0, 9.0, 9.0);
    bool rejected = false;
    try
    {
        editor.set_handles({handle, conflict});
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    if (!rejected)
    {
        return fail("conflicting handles were not rejected");
    }
    if (editor.staged_handles().size() != 1)
    {
        return fail("a rejected set_handles must not overwrite the staged handles");
    }

    // Build and write the mesh of the last successful solve.
    const PatchMesh mesh = tinynurbs::buildPatchMesh(snapshot);
    if (mesh.triangles.empty() || mesh.positions.empty())
    {
        return fail("mesh generation produced an empty mesh");
    }
    if (!tinynurbs::writePatchMeshOBJ(mesh, out_path))
    {
        return fail("could not write the OBJ file");
    }

    // Self-check the written mesh: count its face records and verify that
    // every declared seam has shared edges in the generated topology.
    std::size_t face_count = 0;
    {
        std::FILE *f = std::fopen(out_path.c_str(), "r");
        if (!f)
        {
            return fail("could not reopen the OBJ file");
        }
        char line[256];
        while (std::fgets(line, sizeof(line), f))
        {
            if (line[0] == 'f' && line[1] == ' ')
            {
                ++face_count;
            }
        }
        std::fclose(f);
    }
    if (face_count != mesh.triangles.size())
    {
        return fail("OBJ face count does not match the mesh");
    }

    // Verify seam adjacency: for every declared seam exactly one pair of
    // patches must share welded positions, and no other pair may.
    std::map<std::pair<int, int>, std::size_t> shared_by_pair;
    std::map<int, std::vector<std::pair<int, std::pair<double, double>>>> corners_at;
    for (const auto &tri : mesh.triangles)
    {
        for (int c = 0; c < 3; ++c)
        {
            corners_at[tri.corners[c].position].push_back(
                {tri.corners[c].patch, {tri.corners[c].u, tri.corners[c].v}});
        }
    }
    for (const auto &entry : corners_at)
    {
        std::set<int> patches_here;
        for (const auto &corner : entry.second)
        {
            patches_here.insert(corner.first);
        }
        for (int a : patches_here)
        {
            for (int b : patches_here)
            {
                if (a < b)
                {
                    ++shared_by_pair[{a, b}];
                }
            }
        }
    }
    if (shared_by_pair.size() != seams.size())
    {
        return fail("unexpected number of welded patch pairs");
    }
    for (const auto &seam : seams)
    {
        const std::pair<int, int> key(std::min(seam.patch_a, seam.patch_b),
                                      std::max(seam.patch_a, seam.patch_b));
        if (shared_by_pair.find(key) == shared_by_pair.end())
        {
            return fail("a declared seam has no shared mesh positions");
        }
    }

    std::printf("PATCH_EDITOR_EXAMPLE vertices=%zu triangles=%zu faces=%zu seams=%zu\n",
                mesh.positions.size(), mesh.triangles.size(), face_count, seams.size());
    return 0;
}
