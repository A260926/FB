#pragma once

#include <iosfwd>

#include <clean-core/string_view.hh>

#include <polymesh/Mesh.hh>

namespace polymesh
{
/// Selects how an obj_reader turns the indexed face soup into a half-edge mesh.
enum class obj_read_mode
{
    /// legacy behavior: faces are added line by line, faces that would make
    /// the mesh non-manifold are skipped and counted via error_faces()
    legacy,
    /// analyze the whole file first: winding is made consistent, vertices
    /// carrying disconnected face fans are split, and every source face is
    /// kept. Failures throw std::invalid_argument instead of skipping faces.
    repair
};

template <class ScalarT>
void write_obj(cc::string_view filename, vertex_attribute<std::array<ScalarT, 3>> const& position);
template <class ScalarT>
bool read_obj(cc::string_view filename, Mesh& mesh, vertex_attribute<std::array<ScalarT, 3>>& position);

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
// obj must be manifold
// no negative indices
template <class ScalarT>
struct obj_reader
{
    obj_reader(cc::string_view filename, Mesh& mesh);
    obj_reader(std::istream& in, Mesh& mesh);

    /// read with an explicit mode; obj_read_mode::repair analyzes the whole
    /// file before building the mesh and throws std::invalid_argument on
    /// invalid soup topology instead of skipping faces
    obj_reader(cc::string_view filename, Mesh& mesh, obj_read_mode mode);
    obj_reader(std::istream& in, Mesh& mesh, obj_read_mode mode);

    // get properties of the obj
public:
    vertex_attribute<std::array<ScalarT, 4>> const& get_positions() const { return positions; }
    halfedge_attribute<std::array<ScalarT, 3>> const& get_tex_coords() const { return tex_coords; }
    halfedge_attribute<std::array<ScalarT, 3>> const& get_normals() const { return normals; }

    /// Number of faces that could not be added
    int error_faces() const { return n_error_faces; }

    // source mappings (only promised in obj_read_mode::repair)
public:
    /// output vertex slot -> 0-based source position index
    std::vector<int> const& vertex_source() const { return source_vertices; }
    /// output face slot -> 0-based source face index
    std::vector<int> const& face_source() const { return source_faces; }
    /// output half-edge slot -> 0-based global source corner index
    /// (face-local corner offset plus the per-face global offset);
    /// boundary half-edges map to -1
    std::vector<int> const& halfedge_source() const { return source_halfedges; }

private:
    void parse(std::istream& in, Mesh& mesh);
    void parse_repair(std::istream& in, Mesh& mesh);

    vertex_attribute<std::array<ScalarT, 4>> positions;
    halfedge_attribute<std::array<ScalarT, 3>> tex_coords;
    halfedge_attribute<std::array<ScalarT, 3>> normals;

    std::vector<int> source_vertices;
    std::vector<int> source_faces;
    std::vector<int> source_halfedges;

    int n_error_faces = 0;
};
} // namespace polymesh
