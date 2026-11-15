// Copyright 2011-2022 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/io/write_obj.h"
#include <limits>
#include <unordered_map>
#include "pmp/exceptions.h"

namespace pmp {

void write_obj(const SurfaceMesh& mesh, const std::filesystem::path& file,
               const IOFlags& flags)
{
    FILE* out = fopen(file.string().c_str(), "w");
    if (!out)
        throw IOException("Failed to open file: " + file.string());

    // check if we can write the mesh using 32-bit indices
    const auto uint_max = std::numeric_limits<uint32_t>::max();
    if (mesh.n_vertices() > uint_max)
        throw InvalidInputException(
            "Mesh too large to be written with 32-bit indices.");

    // comment
    fprintf(out, "# OBJ export from PMP\n");

    // write vertices
    auto points = mesh.get_vertex_property<Point>("v:point");
    for (auto v : mesh.vertices())
    {
        const Point& p = points[v];
        fprintf(out, "v %.10f %.10f %.10f\n", p[0], p[1], p[2]);
    }

    auto tex_coords = mesh.get_halfedge_property<TexCoord>("h:tex");
    const bool write_texcoords = tex_coords && flags.use_halfedge_texcoords;

    auto hnormals = mesh.get_halfedge_property<Normal>("h:normal");
    auto hnormal_explicit =
        mesh.get_halfedge_property<bool>("h:normal_explicit");
    const bool write_halfedge_normals =
        flags.use_halfedge_normals && hnormals && hnormal_explicit;

    // write vertex normals only when the per-corner mode is not requested
    auto vnormals = mesh.get_vertex_property<Normal>("v:normal");
    const bool write_vertex_normals =
        !write_halfedge_normals && vnormals && flags.use_vertex_normals;
    if (write_vertex_normals)
    {
        for (auto v : mesh.vertices())
        {
            const Normal& n = vnormals[v];
            fprintf(out, "vn %.10f %.10f %.10f\n", n[0], n[1], n[2]);
        }
    }

    // write texture coordinates
    if (write_texcoords)
    {
        if (mesh.n_halfedges() > uint_max)
            throw InvalidInputException(
                "Mesh too large to be written with 32-bit indices.");

        for (auto h : mesh.halfedges())
        {
            const TexCoord& pt = tex_coords[h];
            fprintf(out, "vt %.10f %.10f\n", pt[0], pt[1]);
        }
    }

    // write per-corner normals: every explicit corner gets its own vn
    // record and therefore an independent normal index, so that a re-read
    // restores exactly the explicit corner mask (identical vectors on two
    // corners are written twice on purpose)
    std::unordered_map<IndexType, uint32_t> normal_index;
    if (write_halfedge_normals)
    {
        if (mesh.n_halfedges() > uint_max)
            throw InvalidInputException(
                "Mesh too large to be written with 32-bit indices.");

        normal_index.reserve(mesh.n_halfedges());
        uint32_t next_index = 1;
        for (auto h : mesh.halfedges())
        {
            if (hnormal_explicit[h])
            {
                const Normal& n = hnormals[h];
                fprintf(out, "vn %.10f %.10f %.10f\n", n[0], n[1], n[2]);
                normal_index.emplace(h.idx(), next_index);
                ++next_index;
            }
        }
    }

    auto smoothing = mesh.get_face_property<int>("f:smoothing_group");
    const bool write_smoothing = write_halfedge_normals && bool(smoothing);
    int current_smoothing = -1;

    // write faces
    for (auto f : mesh.faces())
    {
        if (write_smoothing)
        {
            const int group = smoothing[f];
            if (group != current_smoothing)
            {
                if (group > 0)
                    fprintf(out, "s %d\n", group);
                else
                    fprintf(out, "s off\n");
                current_smoothing = group;
            }
        }

        fprintf(out, "f");

        auto h = mesh.halfedges(f);
        for (auto v : mesh.vertices(f))
        {
            const auto idx = (uint32_t)(v.idx() + 1);
            const uint32_t tidx = (uint32_t)(*h).idx() + 1;
            const auto nit = normal_index.find((*h).idx());
            const bool has_normal =
                write_halfedge_normals && nit != normal_index.end();
            const uint32_t nidx =
                has_normal ? nit->second : uint32_t(0);

            if (write_halfedge_normals)
            {
                if (write_texcoords)
                {
                    if (has_normal)
                        fprintf(out, " %u/%u/%u", idx, tidx, nidx);
                    else
                        fprintf(out, " %u/%u", idx, tidx);
                }
                else if (has_normal)
                {
                    fprintf(out, " %u//%u", idx, nidx);
                }
                else
                {
                    fprintf(out, " %u", idx);
                }
            }
            else if (write_texcoords)
            {
                if (write_vertex_normals)
                {
                    // write vertex index, texCoord index and normal index
                    fprintf(out, " %d/%d/%d", idx, tidx, idx);
                }
                else
                {
                    // write vertex index, texCoord index
                    fprintf(out, " %d/%d/", idx, tidx);
                }
            }
            else if (write_vertex_normals)
            {
                // write vertex index and normal index
                fprintf(out, " %d//%d", idx, idx);
            }
            else
            {
                // write only vertex index
                fprintf(out, " %d", idx);
            }
            ++h;
        }
        fprintf(out, "\n");
    }

    fclose(out);
}

} // namespace pmp
