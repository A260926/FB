//== INCLUDES =================================================================

#include <OpenGP/SurfaceMesh/SurfaceMesh.h>
#include <OpenGP/SurfaceMesh/IO/IO.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include <tuple>
#include <vector>
#include <string>

//=============================================================================
namespace OpenGP {
//=============================================================================

namespace {

// one face corner: 0-based indices, presence flags mark absent components
struct ObjCorner
{
    int v;
    int vt;
    int vn;
    bool has_vt;
    bool has_vn;
};

// resolve a positive (1-based) or negative (relative) OBJ index
int resolve_obj_index(int idx, int count)
{
    if (idx > 0) return idx - 1;
    if (idx < 0) return count + idx;
    return -1;
}

// parse one "v", "v/vt", "v//vn" or "v/vt/vn" token
bool parse_corner(const std::string& token, ObjCorner& c)
{
    c.v = c.vt = c.vn = 0;
    c.has_vt = c.has_vn = false;

    size_t slash1 = token.find('/');
    if (slash1 == std::string::npos)
    {
        c.v = atoi(token.c_str());
        return c.v != 0;
    }

    c.v = atoi(token.substr(0, slash1).c_str());
    if (c.v == 0) return false;

    size_t slash2 = token.find('/', slash1 + 1);
    if (slash2 == std::string::npos)
    {
        // v/vt
        std::string t = token.substr(slash1 + 1);
        if (t.empty()) return false;
        c.vt = atoi(t.c_str());
        if (c.vt == 0) return false;
        c.has_vt = true;
        return true;
    }

    std::string mid = token.substr(slash1 + 1, slash2 - slash1 - 1);
    std::string tail = token.substr(slash2 + 1);
    if (tail.empty()) return false;
    c.vn = atoi(tail.c_str());
    if (c.vn == 0) return false;
    c.has_vn = true;

    if (mid.empty())
    {
        // v//vn
        return true;
    }

    // v/vt/vn
    c.vt = atoi(mid.c_str());
    if (c.vt == 0) return false;
    c.has_vt = true;
    return true;
}

} // anonymous namespace


HEADERONLY_INLINE bool read_obj(SurfaceMesh& mesh, const std::string& filename)
{
    // clear mesh
    mesh.clear();

    std::ifstream in(filename.c_str());
    if (!in) return false;

    std::vector<Vec3> all_tex_coords;    // texture coordinates, in file order
    std::vector<Vec3> all_normals;       // normals, in file order
    std::vector<SurfaceMesh::Vertex> vertices;
    std::vector<ObjCorner> corners;

    SurfaceMesh::Halfedge_property<Vec3> tex_coords;
    SurfaceMesh::Halfedge_property<Vec3> half_normals;
    bool with_tex_coord = false;
    bool with_normal = false;

    std::string line;
    while (std::getline(in, line))
    {
        // strip a possible carriage return (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream iss(line);
        std::string tag;
        if (!(iss >> tag)) continue;

        // vertex position
        if (tag == "v")
        {
            Scalar x, y, z;
            if (!(iss >> x >> y >> z)) return false;
            vertices.push_back(mesh.add_vertex(Vec3(x, y, z)));
        }
        // vertex normal: independent of the number of positions
        else if (tag == "vn")
        {
            Scalar x, y, z;
            if (!(iss >> x >> y >> z)) return false;
            all_normals.push_back(Vec3(x, y, z));
        }
        // texture coordinate (1, 2 or 3 components)
        else if (tag == "vt")
        {
            Scalar x = 0, y = 0, z = 0;
            int n = 0;
            if (iss >> x) ++n;
            if (iss >> y) ++n;
            if (iss >> z) ++n;
            if (n == 0) return false;
            if (n == 2) z = 1;
            all_tex_coords.push_back(Vec3(x, y, z));
        }
        // face
        else if (tag == "f")
        {
            corners.clear();
            std::string token;
            int form = -1; // 0: v, 1: v/vt, 2: v//vn, 3: v/vt/vn
            bool ok = true;

            while (iss >> token)
            {
                ObjCorner c;
                if (!parse_corner(token, c)) { ok = false; break; }

                int this_form;
                if (!c.has_vt && !c.has_vn)       this_form = 0;
                else if (c.has_vt && !c.has_vn)   this_form = 1;
                else if (!c.has_vt && c.has_vn)   this_form = 2;
                else                               this_form = 3;

                // the four forms have to be used consistently within a face
                if (form == -1) form = this_form;
                if (form != this_form) { ok = false; break; }

                c.v  = resolve_obj_index(c.v,  (int)vertices.size());
                c.vt = resolve_obj_index(c.vt, (int)all_tex_coords.size());
                c.vn = resolve_obj_index(c.vn, (int)all_normals.size());

                if (c.v < 0 || c.v >= (int)vertices.size()) { ok = false; break; }
                if (c.has_vt &&
                    (c.vt < 0 || c.vt >= (int)all_tex_coords.size())) { ok = false; break; }
                if (c.has_vn &&
                    (c.vn < 0 || c.vn >= (int)all_normals.size())) { ok = false; break; }

                corners.push_back(c);
            }

            if (!ok || corners.size() < 3) return false;

            std::vector<SurfaceMesh::Vertex> face_verts;
            face_verts.reserve(corners.size());
            for (const ObjCorner& c : corners)
                face_verts.push_back(vertices[c.v]);

            SurfaceMesh::Face f = mesh.add_face(face_verts);
            if (!f.is_valid()) return false;

            if ((form == 1 || form == 3) && !with_tex_coord)
            {
                tex_coords = mesh.halfedge_property<Vec3>("h:texcoord");
                with_tex_coord = true;
            }
            if ((form == 2 || form == 3) && !with_normal)
            {
                half_normals = mesh.halfedge_property<Vec3>("h:normal");
                with_normal = true;
            }

            // fill the per-corner attributes in face traversal order
            SurfaceMesh::Halfedge_around_face_circulator hit = mesh.halfedges(f);
            SurfaceMesh::Halfedge_around_face_circulator hend = hit;
            unsigned int k = 0;
            do
            {
                if (with_tex_coord && (form == 1 || form == 3))
                    tex_coords[*hit] = all_tex_coords[corners[k].vt];
                if (with_normal && (form == 2 || form == 3))
                    half_normals[*hit] = all_normals[corners[k].vn];
                ++k;
                ++hit;
            } while (hit != hend);
        }
    }

    return true;
}


//-----------------------------------------------------------------------------


HEADERONLY_INLINE bool write_obj(const SurfaceMesh& mesh, const std::string& filename)
{
    FILE* out = fopen(filename.c_str(), "w");
    if (!out)
        return false;

    // comment
    fprintf(out, "# OBJ export from SurfaceMesh\n");

    // only live elements are exported; renumber vertices in output order
    std::vector<int> vout(mesh.vertices_size(), -1);
    int vcount = 0;
    for (SurfaceMesh::Vertex_iterator vit = mesh.vertices_begin();
         vit != mesh.vertices_end(); ++vit)
        vout[(*vit).idx()] = ++vcount;

    SurfaceMesh::Vertex_property<Vec3> points = mesh.get_vertex_property<Vec3>("v:point");

    // positions
    for (SurfaceMesh::Vertex_iterator vit = mesh.vertices_begin();
         vit != mesh.vertices_end(); ++vit)
    {
        const Vec3& p = points[*vit];
        fprintf(out, "v %.10f %.10f %.10f\n", p[0], p[1], p[2]);
    }

    SurfaceMesh::Halfedge_property<Vec3> htex =
        mesh.get_halfedge_property<Vec3>("h:texcoord");
    SurfaceMesh::Halfedge_property<Vec3> hnorm =
        mesh.get_halfedge_property<Vec3>("h:normal");
    SurfaceMesh::Vertex_property<Vec3> vnorm =
        mesh.get_vertex_property<Vec3>("v:normal");

    // per-face-corner halfedge normals take priority over vertex normals
    bool use_corner_normals = bool(hnorm);
    bool use_vertex_normals = !use_corner_normals && bool(vnorm);

    // build the per-corner texture index table first (no output yet);
    // only halfedges that are actual face corners are used, so boundary
    // outer halfedges never become render corners
    std::map<std::tuple<float,float,float>, int> tex_index;
    std::vector<int> htex_out(mesh.halfedges_size(), -1);
    if (htex)
    {
        for (SurfaceMesh::Face_iterator fit = mesh.faces_begin();
             fit != mesh.faces_end(); ++fit)
        {
            SurfaceMesh::Halfedge_around_face_circulator hit = mesh.halfedges(*fit);
            SurfaceMesh::Halfedge_around_face_circulator hend = hit;
            do
            {
                const Vec3& t = htex[*hit];
                std::tuple<float,float,float> key(t[0], t[1], t[2]);
                std::map<std::tuple<float,float,float>, int>::iterator
                    it = tex_index.find(key);
                if (it == tex_index.end())
                {
                    int idx = (int)tex_index.size() + 1;
                    tex_index[key] = idx;
                    htex_out[(*hit).idx()] = idx;
                }
                else
                {
                    htex_out[(*hit).idx()] = it->second;
                }
                ++hit;
            } while (hit != hend);
        }
    }

    // build the per-corner normal index table (no output yet); no vn is
    // produced at all when neither corner nor vertex normals exist
    std::map<std::tuple<float,float,float>, int> normal_index;
    std::vector<int> hnormal_out(mesh.halfedges_size(), -1);
    if (use_corner_normals)
    {
        for (SurfaceMesh::Face_iterator fit = mesh.faces_begin();
             fit != mesh.faces_end(); ++fit)
        {
            SurfaceMesh::Halfedge_around_face_circulator hit = mesh.halfedges(*fit);
            SurfaceMesh::Halfedge_around_face_circulator hend = hit;
            do
            {
                const Vec3& n = hnorm[*hit];
                std::tuple<float,float,float> key(n[0], n[1], n[2]);
                std::map<std::tuple<float,float,float>, int>::iterator
                    it = normal_index.find(key);
                if (it == normal_index.end())
                {
                    int idx = (int)normal_index.size() + 1;
                    normal_index[key] = idx;
                    hnormal_out[(*hit).idx()] = idx;
                }
                else
                {
                    hnormal_out[(*hit).idx()] = it->second;
                }
                ++hit;
            } while (hit != hend);
        }
    }

    // texture coordinates, deduplicated and in index order
    std::vector<std::tuple<float,float,float> > tex_values(tex_index.size());
    for (std::map<std::tuple<float,float,float>, int>::const_iterator
             it = tex_index.begin(); it != tex_index.end(); ++it)
        tex_values[it->second - 1] = it->first;
    for (size_t i = 0; i < tex_values.size(); ++i)
        fprintf(out, "vt %.10f %.10f %.10f\n",
                std::get<0>(tex_values[i]), std::get<1>(tex_values[i]),
                std::get<2>(tex_values[i]));

    // normals: corner normals (deduplicated) else the vertex normal block
    if (use_corner_normals)
    {
        std::vector<std::tuple<float,float,float> > nrm_values(normal_index.size());
        for (std::map<std::tuple<float,float,float>, int>::const_iterator
                 it = normal_index.begin(); it != normal_index.end(); ++it)
            nrm_values[it->second - 1] = it->first;
        for (size_t i = 0; i < nrm_values.size(); ++i)
            fprintf(out, "vn %.10f %.10f %.10f\n",
                    std::get<0>(nrm_values[i]), std::get<1>(nrm_values[i]),
                    std::get<2>(nrm_values[i]));
    }
    else if (use_vertex_normals)
    {
        for (SurfaceMesh::Vertex_iterator vit = mesh.vertices_begin();
             vit != mesh.vertices_end(); ++vit)
        {
            const Vec3& n = vnorm[*vit];
            fprintf(out, "vn %.10f %.10f %.10f\n", n[0], n[1], n[2]);
        }
    }

    // faces: each face consistently uses one of the four OBJ forms
    for (SurfaceMesh::Face_iterator fit = mesh.faces_begin();
         fit != mesh.faces_end(); ++fit)
    {
        fprintf(out, "f");
        SurfaceMesh::Vertex_around_face_circulator fvit = mesh.vertices(*fit);
        SurfaceMesh::Vertex_around_face_circulator fvend = fvit;
        SurfaceMesh::Halfedge_around_face_circulator fhit = mesh.halfedges(*fit);
        do
        {
            int vi = vout[(*fvit).idx()];
            if (htex)
            {
                int ti = htex_out[(*fhit).idx()];
                if (use_corner_normals)
                    fprintf(out, " %d/%d/%d", vi, ti, hnormal_out[(*fhit).idx()]);
                else if (use_vertex_normals)
                    fprintf(out, " %d/%d/%d", vi, ti, vi);
                else
                    fprintf(out, " %d/%d", vi, ti);
            }
            else
            {
                if (use_corner_normals)
                    fprintf(out, " %d//%d", vi, hnormal_out[(*fhit).idx()]);
                else if (use_vertex_normals)
                    fprintf(out, " %d//%d", vi, vi);
                else
                    fprintf(out, " %d", vi);
            }
            ++fhit;
        } while (++fvit != fvend);
        fprintf(out, "\n");
    }

    fclose(out);
    return true;
}


//=============================================================================
} // namespace OpenGP
//=============================================================================
