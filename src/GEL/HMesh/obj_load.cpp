/* ----------------------------------------------------------------------- *
 * This file is part of GEL, http://www.imm.dtu.dk/GEL
 * Copyright (C) the authors and DTU Informatics
 * For license and list of authors, see ../../doc/intro.pdf
 * ----------------------------------------------------------------------- */

#include <sstream>
#include <fstream>
#include <GEL/HMesh/load.h>
#include <GEL/HMesh/obj_load.h>
#include <GEL/HMesh/Manifold.h>
#include <GEL/HMesh/cleanup.h>
#include <GEL/HMesh/AttributedManifold.h>
#include <cmath>

using namespace std;
using namespace CGLA;

namespace HMesh
{
    using std::string;
    
    void right_trim(std::string& str) {
        while((!str.empty()) && isspace(str.back()))
            str.pop_back();
    }
    
    istream& get_multi_line(istream& is, string& buf) {
        getline(is,buf);
        right_trim(buf);
        while(!buf.empty() && buf.back() == '\\') {
            buf.pop_back();
            string continuation;
            getline(is, continuation);
            right_trim(continuation);
            buf += continuation;
        }
        return is;
    }
    
    bool obj_load(const std::string& filename, Manifold& m, VertexAttributeVector<int>& orig_vertex_indices)
    {
        ifstream obj_file(filename.data());
        
        if(obj_file)
        {
            string buf;
            vector<Vec3d> vertices;
            vector<int> faces;
            vector<int> indices;
            while(get_multi_line(obj_file,buf))
            {
                istringstream iss(std::move(buf));
                string code;
                if(iss>>code) {
                    if(code == "v") {
                        Vec3d v;
                        iss >> v;
                        vertices.push_back(v);
                    }
                    else if (code == "f") {
                        string str;
                        int ctr = 0;
                        while(iss>>str) {
                            int v,t,n;
                            if(sscanf(str.c_str(), "%d/%d/%d", &v, &t, &n)==3 ||
                               sscanf(str.c_str(), "%d//%d", &v, &n)==2 ||
                               sscanf(str.c_str(), "%d/%d", &v, &t)==2 ||
                               sscanf(str.c_str(), "%d", &v)==1) {
                                indices.push_back(v>0?v-1:vertices.size()+v);
                                ++ctr;
                            }
                        }
                        faces.push_back(ctr);
                    }
                }
            }
//            cout << "Loaded " << filename << " : " << vertices.size() << " vertices and "
//            << faces.size() << " faces"<< endl;
            m.clear();
            
            orig_vertex_indices = build(m, vertices.size(),
                  reinterpret_cast<double*>(&vertices[0]),
                  faces.size(),
                  &faces[0],
                  &indices[0]);
            
            return true;
        }
        return false;
    }
    
    bool obj_load(const string& filename, Manifold& m) {
        VertexAttributeVector<int> orig_vertex_indices;
        return obj_load(filename, m, orig_vertex_indices);
    }

    namespace
    {
        struct AttrCornerSpec
        {
            int v;
            int t;
            int n;
            bool has_t;
            bool has_n;
        };

        // Parse one OBJ face corner token (v, v/vt, v//vn, v/vt/vn),
        // resolving negative indices against the counts seen so far.
        bool parse_attr_corner(const string& token, size_t nv, size_t nt, size_t nn, AttrCornerSpec& out)
        {
            string parts[3];
            int nparts = 0;
            size_t begin = 0;
            while (true) {
                if (nparts == 3)
                    return false;
                size_t slash = token.find('/', begin);
                parts[nparts++] = token.substr(begin, slash == string::npos ? string::npos : slash - begin);
                if (slash == string::npos)
                    break;
                begin = slash + 1;
            }

            auto resolve = [](const string& s, size_t count, int& idx) -> bool {
                if (s.empty())
                    return false;
                try {
                    size_t used = 0;
                    int raw = stoi(s, &used);
                    if (used != s.size())
                        return false;
                    if (raw > 0)
                        idx = raw - 1;
                    else {
                        // -1 refers to the most recently defined element.
                        long long resolved = static_cast<long long>(count) + raw;
                        if (resolved < 0)
                            return false;
                        idx = static_cast<int>(resolved);
                    }
                    return idx >= 0 && static_cast<size_t>(idx) < count;
                }
                catch (...) {
                    return false;
                }
            };

            out = AttrCornerSpec{};
            if (!resolve(parts[0], nv, out.v))
                return false;
            if (nparts >= 2 && !parts[1].empty()) {
                if (!resolve(parts[1], nt, out.t))
                    return false;
                out.has_t = true;
            }
            if (nparts == 3 && !parts[2].empty()) {
                if (!resolve(parts[2], nn, out.n))
                    return false;
                out.has_n = true;
            }
            return true;
        }

        bool finite_vec3(const Vec3d& v)
        {
            return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
        }
    }

    bool obj_load(const string& filename, AttributedManifold& am)
    {
        try {
        ifstream obj_file(filename);
        if (!obj_file)
            return false;

        vector<Vec3d> vertices;
        vector<Vec2d> texverts;
        vector<Vec3d> normverts;
        // One entry per face: the ordered list of its corners.
        vector<vector<AttrCornerSpec>> faces;

        string buf;
        while (getline(obj_file, buf)) {
            if (!buf.empty() && buf.back() == '\r')
                buf.pop_back();
            istringstream iss(buf);
            string code;
            if (!(iss >> code))
                continue;
            if (code == "v") {
                Vec3d v;
                iss >> v[0] >> v[1] >> v[2];
                if (iss.fail() || !finite_vec3(v))
                    return false;
                vertices.push_back(v);
            }
            else if (code == "vt") {
                Vec2d t;
                iss >> t[0] >> t[1];
                if (iss.fail() || !(isfinite(t[0]) && isfinite(t[1])))
                    return false;
                texverts.push_back(t);
            }
            else if (code == "vn") {
                Vec3d n;
                iss >> n[0] >> n[1] >> n[2];
                if (iss.fail() || !finite_vec3(n) || sqr_length(n) <= 0.0)
                    return false;
                normverts.push_back(n);
            }
            else if (code == "f") {
                vector<AttrCornerSpec> corners;
                string token;
                while (iss >> token) {
                    AttrCornerSpec spec;
                    if (!parse_attr_corner(token, vertices.size(), texverts.size(), normverts.size(), spec))
                        return false;
                    corners.push_back(spec);
                }
                if (corners.size() < 3)
                    return false;
                faces.push_back(std::move(corners));
            }
        }
        if (vertices.empty() || faces.empty())
            return false;

        AttributedManifold built;
        Manifold& m = built.mesh;
        VertexAttributeVector<int> cluster_id;

        for (const auto& corners : faces) {
            vector<Vec3d> pts;
            pts.reserve(corners.size());
            for (const auto& c : corners)
                pts.push_back(vertices[c.v]);
            FaceID f = m.add_face(pts);
            int j = 0;
            circulate_face_ccw(m, f, [&](VertexID v) {
                cluster_id[v] = corners[j].v;
                ++j;
            });
        }

        // Stitch the separately created faces along shared geometry vertices,
        // then compact so all handles become contiguous.
        stitch_mesh(m, cluster_id);
        IDRemap remap;
        m.cleanup(remap);
        cluster_id.cleanup(remap.vmap);

        // Faces keep their creation order through stitching and compaction,
        // so face k still describes input face k in its original corner order.
        size_t fi = 0;
        for (FaceID f : m.faces()) {
            const vector<AttrCornerSpec>& corners = faces[fi++];
            size_t ci = 0;
            for (Walker w = m.walker(f); !w.full_circle(); w = w.circulate_face_ccw()) {
                const AttrCornerSpec& spec = corners[ci++];
                HalfEdgeID h = w.halfedge();
                if (spec.has_t) {
                    built.texcoords[h] = texverts[spec.t];
                    built.has_texcoord[h] = 1;
                }
                if (spec.has_n) {
                    built.normals[h] = normverts[spec.n];
                    built.has_normal[h] = 1;
                }
            }
            if (ci != corners.size())
                return false;
        }
        if (fi != faces.size())
            return false;

        am = std::move(built);
        return true;
        }
        catch (...) {
            // Any failure leaves the caller's object untouched.
            return false;
        }
    }

}
