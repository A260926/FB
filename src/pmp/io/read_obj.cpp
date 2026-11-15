// Copyright 2011-2022 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/io/read_obj.h"
#include "pmp/exceptions.h"

namespace pmp {

void read_obj(SurfaceMesh& mesh, const std::filesystem::path& file)
{
    std::array<char, 200> s;
    float x, y, z, r, g, b;
    std::vector<Vertex> vertices;
    std::vector<TexCoord> all_tex_coords;   // individual texture coordinates
    std::vector<Normal> all_normals;        // individual normal vectors
    std::vector<int> halfedge_tex_idx;      // texture coords sorted for halfedges
    std::vector<int> halfedge_normal_idx;   // normal indices sorted for halfedges
    HalfedgeProperty<TexCoord> tex_coords =
        mesh.halfedge_property<TexCoord>("h:tex");
    bool with_tex_coord = false;
    HalfedgeProperty<Normal> hnormal =
        mesh.halfedge_property<Normal>("h:normal");
    HalfedgeProperty<bool> hnormal_explicit =
        mesh.halfedge_property<bool>("h:normal_explicit", false);
    bool with_normal = false;
    FaceProperty<int> smoothing =
        mesh.face_property<int>("f:smoothing_group", 0);
    int current_smoothing = 0;
    VertexProperty<Color> colors;

    // open file (in ASCII mode)
    FILE* in = fopen(file.string().c_str(), "r");
    if (!in)
        throw IOException("Failed to open file: " + file.string());

    // clear line once
    memset(s.data(), 0, 200);

    // parse line by line (currently only supports vertex positions & faces
    while (in && !feof(in) && fgets(s.data(), 200, in))
    {
        // comment
        if (s[0] == '#' || isspace(s[0]))
            continue;

        // vertex
        else if (strncmp(s.data(), "v ", 2) == 0)
        {
            const int n =
                sscanf(s.data(), "v %f %f %f %f %f %f", &x, &y, &z, &r, &g, &b);
            if (n >= 3)
            {
                const Vertex v = mesh.add_vertex(Point(x, y, z));
                if (n >= 6)
                {
                    if (!colors)
                        colors = mesh.vertex_property<Color>("v:color");
                    colors[v] = Color(r, g, b);
                }
            }
        }

        // normal
        else if (strncmp(s.data(), "vn ", 3) == 0)
        {
            if (sscanf(s.data(), "vn %f %f %f", &x, &y, &z) == 3)
            {
                all_normals.emplace_back(x, y, z);
            }
        }

        // texture coordinate
        else if (strncmp(s.data(), "vt ", 3) == 0)
        {
            if (sscanf(s.data(), "vt %f %f", &x, &y))
            {
                all_tex_coords.emplace_back(x, y);
            }
        }

        // smoothing group
        else if (strncmp(s.data(), "s ", 2) == 0)
        {
            char token[64] = {0};
            if (sscanf(s.data(), "s %63s", token) == 1)
            {
                if (strcmp(token, "off") == 0)
                    current_smoothing = 0;
                else
                {
                    const int group = atoi(token);
                    current_smoothing = group > 0 ? group : 0;
                }
            }
        }

        // face
        else if (strncmp(s.data(), "f ", 2) == 0)
        {
            int component(0);
            bool end_of_vertex(false);
            char *p0, *p1(s.data() + 1);

            vertices.clear();
            halfedge_tex_idx.clear();
            halfedge_normal_idx.clear();

            // skip white-spaces
            while (*p1 == ' ')
                ++p1;

            while (p1)
            {
                p0 = p1;

                // overwrite next separator

                // skip '/', '\n', ' ', '\0', '\r' <-- don't forget Windows
                while (*p1 != '/' && *p1 != '\r' && *p1 != '\n' && *p1 != ' ' &&
                       *p1 != '\0')
                    ++p1;

                // detect end of vertex
                if (*p1 != '/')
                {
                    end_of_vertex = true;
                }

                // replace separator by '\0'
                if (*p1 != '\0')
                {
                    *p1 = '\0';
                    p1++; // point to next token
                }

                // detect end of line and break
                if (*p1 == '\0' || *p1 == '\n')
                {
                    p1 = nullptr;
                }

                // read next vertex component
                if (*p0 != '\0')
                {
                    switch (component)
                    {
                        case 0: // vertex
                        {
                            int idx = atoi(p0);
                            if (idx < 0)
                                idx = mesh.n_vertices() + idx + 1;
                            vertices.emplace_back(idx - 1);
                            break;
                        }
                        case 1: // texture coord
                        {
                            int idx = atoi(p0);
                            if (idx < 0)
                                idx =
                                    static_cast<int>(all_tex_coords.size()) +
                                    idx + 1;
                            halfedge_tex_idx.push_back(idx - 1);
                            with_tex_coord = true;
                            break;
                        }
                        case 2: // normal
                        {
                            int idx = atoi(p0);
                            if (idx < 0)
                                idx = static_cast<int>(all_normals.size()) +
                                      idx + 1;
                            halfedge_normal_idx.push_back(idx - 1);
                            with_normal = true;
                            break;
                        }
                    }
                }

                ++component;

                if (end_of_vertex)
                {
                    component = 0;
                    end_of_vertex = false;
                    // pad the per-corner index domains so that corners which
                    // omit a texture or normal index stay aligned
                    if (halfedge_tex_idx.size() < vertices.size())
                        halfedge_tex_idx.push_back(-1);
                    if (halfedge_normal_idx.size() < vertices.size())
                        halfedge_normal_idx.push_back(-1);
                }
            }

            Face f;
            try
            {
                f = mesh.add_face(vertices);
            }
            catch (const TopologyException& e)
            {
                std::cerr << e.what();
            }

            if (f.is_valid())
            {
                smoothing[f] = current_smoothing;

                // add texture coordinates
                if (with_tex_coord)
                {
                    auto h_fit = mesh.halfedges(f);
                    auto h_end = h_fit;
                    unsigned v_idx = 0;
                    do
                    {
                        const int tidx = halfedge_tex_idx.at(v_idx);
                        if (tidx >= 0)
                            tex_coords[*h_fit] = all_tex_coords.at(tidx);
                        ++v_idx;
                        ++h_fit;
                    } while (h_fit != h_end);
                }

                // add explicit corner normals
                if (with_normal && !halfedge_normal_idx.empty())
                {
                    auto h_fit = mesh.halfedges(f);
                    auto h_end = h_fit;
                    unsigned v_idx = 0;
                    do
                    {
                        const int nidx = halfedge_normal_idx.at(v_idx);
                        if (nidx >= 0)
                        {
                            hnormal[*h_fit] = all_normals.at(nidx);
                            hnormal_explicit[*h_fit] = true;
                        }
                        ++v_idx;
                        ++h_fit;
                    } while (h_fit != h_end);
                }
            }
        }
        // clear line
        memset(s.data(), 0, 200);
    }

    // if there are no textures, delete texture property!
    if (!with_tex_coord)
    {
        mesh.remove_halfedge_property(tex_coords);
    }

    // if there are no normals, delete the normal properties!
    if (!with_normal)
    {
        mesh.remove_halfedge_property(hnormal);
        mesh.remove_halfedge_property(hnormal_explicit);
    }

    fclose(in);
}

} // namespace pmp
