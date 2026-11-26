#pragma once

#include <iosfwd>
#include <vector>

#include <clean-core/string_view.hh>

#include <polymesh/Mesh.hh>

namespace polymesh
{
template <class ScalarT>
void write_obj(cc::string_view filename, vertex_attribute<std::array<ScalarT, 3>> const& position);
template <class ScalarT>
bool read_obj(cc::string_view filename, Mesh& mesh, vertex_attribute<std::array<ScalarT, 3>>& position);

/// Selects how obj_reader treats conflicting polygon soup input.
enum class obj_read_mode
{
    /// legacy behavior: faces are added line by line, conflicting faces are skipped
    legacy,
    /// analyze the whole file first, repair winding and split disconnected face
    /// fans, then build a manifold half-edge mesh containing every source face
    repair,
};

template <class ScalarT>
struct obj_writer
{
    obj_writer(cc::string_view filename);
    obj_writer(std::ostream& out);
    ~obj_writer();

    void write_object_name(cc::string_view object_name);
    void write_mesh(vertex_attribute<std::array<ScalarT, 4>> const& position,
                    halfedge_attribute<std::array<ScalarT, 3>> const* tex_coord = nullptr,
                    halfedge_attribute<std::array<ScalarT, 3>> const* normal = nullptr);
    void write_mesh(vertex_attribute<std::array<ScalarT, 3>> const& position,
                    vertex_attribute<std::array<ScalarT, 2>> const* tex_coord = nullptr,
                    vertex_attribute<std::array<ScalarT, 3>> const* normal = nullptr);

    // TODO: tex coords and normals as half-edge attributes

private:
    std::ostream* tmp_out = nullptr;
    std::ostream* out = nullptr;

    int vertex_idx = 1;
    int texture_idx = 1;
    int normal_idx = 1;
};

// clears the given mesh before adding data
// legacy mode: obj must be manifold, no negative indices, non-manifold faces are skipped
// repair mode: the whole soup is analyzed first; winding is made consistent and
// disconnected face fans are split onto duplicated vertices. Throws
// std::invalid_argument on malformed or contradictory input.
template <class ScalarT>
struct obj_reader
{
    obj_reader(cc::string_view filename, Mesh& mesh, obj_read_mode mode = obj_read_mode::legacy);
    obj_reader(std::istream& in, Mesh& mesh, obj_read_mode mode = obj_read_mode::legacy);

    // get properties of the obj
public:
    vertex_attribute<std::array<ScalarT, 4>> const& get_positions() const { return positions; }
    halfedge_attribute<std::array<ScalarT, 3>> const& get_tex_coords() const { return tex_coords; }
    halfedge_attribute<std::array<ScalarT, 3>> const& get_normals() const { return normals; }

    /// Number of faces that could not be added (always zero in repair mode)
    int error_faces() const { return n_error_faces; }

    /// Source mappings, only promised in obj_read_mode::repair.
    /// Output slot index -> zero-based source position/face/corner.
    /// Source corners are accumulated per face in file order, restarting at
    /// zero inside each face. Boundary halfedges map to -1.
    std::vector<int> const& vertex_source() const { return source_vertices; }
    std::vector<int> const& face_source() const { return source_faces; }
    std::vector<int> const& halfedge_source() const { return source_halfedges; }

private:
    void parse(std::istream& in, Mesh& mesh);

    vertex_attribute<std::array<ScalarT, 4>> positions;
    halfedge_attribute<std::array<ScalarT, 3>> tex_coords;
    halfedge_attribute<std::array<ScalarT, 3>> normals;

    std::vector<int> source_vertices;
    std::vector<int> source_faces;
    std::vector<int> source_halfedges;

    int n_error_faces = 0;
    obj_read_mode read_mode = obj_read_mode::legacy;
};
} // namespace polymesh
