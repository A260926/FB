#include "obj.hh"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#include <polymesh/algorithms/soup_repair.hh>

namespace polymesh
{
namespace
{
/// Everything an OBJ file contributes, collected before any mesh is built.
/// Both read modes tokenize the file through this structure.
template <class ScalarT>
struct obj_soup
{
    std::vector<std::array<ScalarT, 4>> positions;
    std::vector<std::array<ScalarT, 3>> tex_coords;
    std::vector<std::array<ScalarT, 3>> normals;
    std::vector<soup_face> faces;
    /// 1-based vertex chains from 'l' records, only replayed in legacy mode
    std::vector<std::vector<int>> lines;
};

/// Parses one "v/vt/vn" face token. OBJ indices are 1-based; a missing
/// component is -1. Zero and negative values are mapped to sentinels without
/// throwing so legacy parsing stays permissive; repair mode validates them
/// through repair_polygon_soup.
soup_corner parse_face_token(std::string const& token)
{
    // split on '/', keeping empty fields so "1//2" is v=1, vt absent, n=2
    std::vector<std::string> fields;
    fields.push_back({});
    for (char c : token)
    {
        if (c == '/')
            fields.push_back({});
        else
            fields.back().push_back(c);
    }

    auto const parse_one = [](std::string const& field, int& target) {
        if (field.empty())
            return;
        std::istringstream ss(field);
        int value = 0;
        ss >> value;
        if (value > 0)
            target = value - 1;
        else if (value == 0)
            target = -2;
    };

    soup_corner corner;
    if (!fields.empty())
    {
        int vertex = 0;
        std::istringstream ss(fields[0]);
        ss >> vertex;
        corner.vertex = vertex > 0 ? vertex - 1 : -1;
    }
    if (fields.size() >= 2)
        parse_one(fields[1], corner.texcoord);
    if (fields.size() >= 3)
        parse_one(fields[2], corner.normal);

    return corner;
}

template <class ScalarT>
obj_soup<ScalarT> collect_obj_soup(std::istream& in)
{
    obj_soup<ScalarT> soup;

    std::string line_s;
    auto line_nr = 0;
    while (std::getline(in, line_s))
    {
        ++line_nr;
        while (!line_s.empty() && (line_s.back() == '\r' || line_s.back() == ' ' || line_s.back() == '\t'))
            line_s.pop_back();

        std::istringstream line(line_s);
        std::string type;
        line >> type;

        if (type.empty() || type[0] == '#')
            continue;

        if (type == "v")
        {
            std::array<ScalarT, 4> p;
            p[3] = ScalarT(1);
            line >> p[0];
            line >> p[1];
            line >> p[2];
            ScalarT w;
            line >> w;
            if (line.good())
                p[3] = w;
            soup.positions.push_back(p);
        }
        else if (type == "vt")
        {
            std::array<ScalarT, 3> t;
            t[2] = ScalarT(1);
            line >> t[0];
            line >> t[1];
            ScalarT z;
            line >> z;
            if (line.good())
                t[2] = z;
            soup.tex_coords.push_back(t);
        }
        else if (type == "vn")
        {
            std::array<ScalarT, 3> n;
            line >> n[0];
            line >> n[1];
            line >> n[2];
            soup.normals.push_back(n);
        }
        else if (type == "f")
        {
            soup_face face;
            std::string token;
            while (line >> token)
                face.corners.push_back(parse_face_token(token));
            soup.faces.push_back(std::move(face));
        }
        else if (type == "l")
        {
            std::vector<int> chain;
            int index = 0;
            while (line >> index)
                chain.push_back(index);
            if (!chain.empty())
                soup.lines.push_back(std::move(chain));
        }
        else if (type == "s" || type == "o" || type == "g" || type == "usemtl" || type == "mtllib")
        {
            continue;
        }
        else
        {
            std::cerr << "Unable to parse line " << line_nr << ": " << line_s << std::endl;
        }
    }

    return soup;
}

/// Builds the repaired half-edge mesh. The soup analysis has already
/// guaranteed manifoldness, so edges/faces can be created through the regular
/// low-level insertion path and boundary rings are maintained automatically.
template <class ScalarT>
void build_repaired_mesh(obj_soup<ScalarT> const& soup, repaired_soup const& repaired, Mesh& mesh,
                         vertex_attribute<std::array<ScalarT, 4>>& positions,
                         halfedge_attribute<std::array<ScalarT, 3>>& tex_coords,
                         halfedge_attribute<std::array<ScalarT, 3>>& normals,
                         std::vector<int>& source_vertices, std::vector<int>& source_faces, std::vector<int>& source_halfedges)
{
    auto ll = low_level_api(mesh);

    auto const vertex_count = repaired.vertex_count();
    auto const face_count = repaired.face_count();

    ll.reserve_vertices(vertex_count);
    ll.reserve_faces(face_count);
    // every face corner introduces at most one directed halfedge; shared
    // edges only reduce the final count
    ll.reserve_halfedges(2 * (int)repaired.halfedge_source.size());

    for (auto i = 0; i < vertex_count; ++i)
    {
        auto const v = ll.add_vertex();
        positions[v] = soup.positions[(size_t)repaired.vertex_source[i]];
    }
    source_vertices = repaired.vertex_source;
    source_faces.reserve(face_count);

    // (output halfedge index, global source corner of its vertex_to corner)
    std::vector<std::pair<int, int>> halfedge_origins;
    halfedge_origins.reserve(repaired.halfedge_source.size());

    std::vector<halfedge_index> loop;
    auto flat_cursor = 0;

    for (auto f = 0; f < face_count; ++f)
    {
        auto const& out_face = repaired.faces[f];
        auto const& out_corners = repaired.corners[f];
        auto const k = (int)out_face.size();

        loop.clear();
        loop.reserve(k);

        for (auto i = 0; i < k; ++i)
        {
            auto const from = vertex_index(out_face[i]);
            auto const to = vertex_index(out_face[(i + 1) % k]);
            auto const h = ll.add_or_get_halfedge(from, to);
            loop.push_back(h);

            // attributes and origin follow the vertex_to corner; unspecified
            // components keep the attribute's default value, as in legacy mode
            auto const& to_corner = out_corners[(i + 1) % k];
            if (to_corner.texcoord >= 0)
                tex_coords[h] = soup.tex_coords[(size_t)to_corner.texcoord];
            if (to_corner.normal >= 0)
                normals[h] = soup.normals[(size_t)to_corner.normal];

            halfedge_origins.push_back({h.value, repaired.halfedge_source[(size_t)flat_cursor + (i + 1) % k]});
        }

        ll.add_face(loop.data(), k);
        source_faces.push_back(repaired.face_source[f]);
        flat_cursor += k;
    }

    // boundary halfedges keep the promised -1 origin
    source_halfedges.assign((size_t)ll.size_all_halfedges(), -1);
    for (auto const& entry : halfedge_origins)
        source_halfedges[(size_t)entry.first] = entry.second;
}
} // namespace

template <class ScalarT>
void write_obj(cc::string_view filename, vertex_attribute<std::array<ScalarT, 3>> const& position)
{
    obj_writer<ScalarT> obj(filename);
    obj.write_mesh(position);
}

template <class ScalarT>
bool read_obj(cc::string_view filename, Mesh& mesh, vertex_attribute<std::array<ScalarT, 3>>& position)
{
    obj_reader<ScalarT> reader(filename, mesh);
    position = reader.get_positions().map([](std::array<ScalarT, 4> const& p) { return std::array<ScalarT, 3>{{p[0], p[1], p[2]}}; });
    return reader.error_faces() == 0;
}

template <class ScalarT>
obj_writer<ScalarT>::obj_writer(cc::string_view filename)
{
    tmp_out = new std::ofstream(std::string(filename.begin(), filename.end()));
    out = tmp_out;
}

template <class ScalarT>
obj_writer<ScalarT>::obj_writer(std::ostream& out)
{
    this->out = &out;
}

template <class ScalarT>
obj_writer<ScalarT>::~obj_writer()
{
    delete tmp_out;
}

template <class ScalarT>
void obj_writer<ScalarT>::write_object_name(cc::string_view object_name)
{
    *out << "o " << std::string_view(object_name.data(), object_name.size()) << "\n";
}

template <class ScalarT>
void obj_writer<ScalarT>::write_mesh(vertex_attribute<std::array<ScalarT, 3>> const& position,
                                     vertex_attribute<std::array<ScalarT, 2>> const* tex_coord,
                                     vertex_attribute<std::array<ScalarT, 3>> const* normal)
{
    auto const& mesh = position.mesh();

    auto base_v = vertex_idx;
    auto base_t = texture_idx;
    auto base_n = normal_idx;

    for (auto v : mesh.all_vertices())
    {
        auto pos = v[position];
        *out << "v " << pos[0] << " " << pos[1] << " " << pos[2] << "\n";
        ++vertex_idx;
    }

    if (tex_coord)
        for (auto v : mesh.all_vertices())
        {
            auto t = v[*tex_coord];
            *out << "vt " << t[0] << " " << t[1] << "\n";
            ++texture_idx;
        }

    if (normal)
        for (auto v : mesh.all_vertices())
        {
            auto n = v[*normal];
            *out << "vn " << n[0] << " " << n[1] << " " << n[2] << "\n";
            ++normal_idx;
        }

    for (auto f : mesh.faces())
    {
        *out << "f";
        for (auto v : f.vertices())
        {
            auto i = v.idx.value;
            *out << " ";
            *out << base_v + i;
            if (tex_coord || normal)
                *out << "/";
            if (tex_coord)
                *out << base_t + i;
            if (normal)
            {
                *out << "/";
                *out << base_n + i;
            }
        }
        *out << "\n";
    }
}

template <class ScalarT>
void obj_writer<ScalarT>::write_mesh(vertex_attribute<std::array<ScalarT, 4>> const& position,
                                     halfedge_attribute<std::array<ScalarT, 3>> const* tex_coord,
                                     halfedge_attribute<std::array<ScalarT, 3>> const* normal)
{
    auto const& mesh = position.mesh();

    auto base_v = vertex_idx;
    auto base_t = texture_idx;
    auto base_n = normal_idx;

    for (auto v : mesh.all_vertices())
    {
        auto pos = v[position];
        *out << "v " << pos[0] << " " << pos[1] << " " << pos[2] << " " << pos[3] << "\n";
        ++vertex_idx;
    }

    if (tex_coord)
        for (auto h : mesh.all_halfedges())
        {
            auto t = h[*tex_coord];
            *out << "vt " << t[0] << " " << t[1] << " " << t[2] << "\n";
            ++texture_idx;
        }

    if (normal)
        for (auto h : mesh.all_halfedges())
        {
            auto n = h[*normal];
            *out << "vn " << n[0] << " " << n[1] << " " << n[2] << "\n";
            ++normal_idx;
        }

    for (auto f : mesh.faces())
    {
        *out << "f";
        for (auto h : f.halfedges())
        {
            auto vi = int(h.vertex_to());
            auto hi = int(h);
            *out << " ";
            *out << base_v + vi;
            if (tex_coord || normal)
                *out << "/";
            if (tex_coord)
                *out << base_t + hi;
            if (normal)
            {
                *out << "/";
                *out << base_n + hi;
            }
        }
        *out << "\n";
    }
}

template <class ScalarT>
obj_reader<ScalarT>::obj_reader(cc::string_view filename, Mesh& mesh, obj_read_mode mode)
  : positions(mesh), tex_coords(mesh), normals(mesh), read_mode(mode)
{
    auto const filename_str = std::string(filename.begin(), filename.end());
    std::ifstream file(filename_str);
    if (!file.good())
        std::cerr << "Cannot read from file `" << filename_str << "'" << std::endl;
    else
        parse(file, mesh);
}

template <class ScalarT>
obj_reader<ScalarT>::obj_reader(std::istream& in, Mesh& mesh, obj_read_mode mode)
  : positions(mesh), tex_coords(mesh), normals(mesh), read_mode(mode)
{
    parse(in, mesh);
}

template <class ScalarT>
void obj_reader<ScalarT>::parse(std::istream& in, Mesh& mesh)
{
    mesh.clear();
    source_vertices.clear();
    source_faces.clear();
    source_halfedges.clear();
    n_error_faces = 0;

    auto soup = collect_obj_soup<ScalarT>(in);

    // Both modes run the same production connectivity analysis. In legacy
    // mode its strict diagnostics are advisory only: the tolerant
    // line-by-line insertion below keeps the historical skip-on-conflict
    // behavior.
    auto repaired = std::unique_ptr<repaired_soup>();
    try
    {
        repaired.reset(new repaired_soup(repair_polygon_soup(soup.faces, (int)soup.positions.size(), (int)soup.tex_coords.size(), (int)soup.normals.size())));
    }
    catch (std::invalid_argument const&)
    {
        if (read_mode == obj_read_mode::repair)
            throw;
    }

    if (read_mode == obj_read_mode::repair)
    {
        build_repaired_mesh(soup, *repaired, mesh, positions, tex_coords, normals, source_vertices, source_faces, source_halfedges);
        return;
    }

    for (auto const& p : soup.positions)
    {
        auto v = mesh.vertices().add();
        positions[v] = p;
    }

    std::vector<vertex_index> poly_vs;
    std::vector<halfedge_handle> poly_hs;

    for (auto const& face : soup.faces)
    {
        auto const k = (int)face.corners.size();
        if (k < 3)
        {
            std::cerr << "faces with less than 3 vertices are not supported. Use lines instead." << std::endl;
            continue;
        }

        poly_vs.clear();
        poly_vs.reserve(k);
        for (auto const& c : face.corners)
            poly_vs.push_back(vertex_index(c.vertex));

        if (!mesh.faces().can_add(poly_vs))
        {
            ++n_error_faces;
            continue;
        }

        poly_hs.resize(k);
        for (auto i = 0; i < k; ++i)
        {
            auto const& c0 = face.corners[i];
            auto const& c1 = face.corners[(i + 1) % k];
            auto hh = mesh.halfedges().add_or_get(mesh.handle_of(vertex_index(c0.vertex)), mesh.handle_of(vertex_index(c1.vertex)));
            poly_hs[i] = hh;

            if (c1.texcoord >= 0 && c1.texcoord < (int)soup.tex_coords.size())
                tex_coords[hh] = soup.tex_coords[(size_t)c1.texcoord];
            if (c1.normal >= 0 && c1.normal < (int)soup.normals.size())
                normals[hh] = soup.normals[(size_t)c1.normal];
        }

        mesh.faces().add(poly_hs);
    }

    for (auto const& chain : soup.lines)
        for (auto i = 0u; i + 1 < chain.size(); ++i)
            mesh.edges().add_or_get(mesh[vertex_index(chain[i] - 1)], mesh[vertex_index(chain[i + 1] - 1)]);

    if (n_error_faces > 0)
        std::cerr << "skipped " << n_error_faces << " face(s) because mesh would become non-manifold" << std::endl;
}

template void write_obj<float>(cc::string_view filename, vertex_attribute<std::array<float, 3>> const& position);
template bool read_obj<float>(cc::string_view filename, Mesh& mesh, vertex_attribute<std::array<float, 3>>& position);
template struct obj_reader<float>;
template struct obj_writer<float>;

template void write_obj<double>(cc::string_view filename, vertex_attribute<std::array<double, 3>> const& position);
template bool read_obj<double>(cc::string_view filename, Mesh& mesh, vertex_attribute<std::array<double, 3>>& position);
template struct obj_reader<double>;
template struct obj_writer<double>;

} // namespace polymesh
