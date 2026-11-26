// Minimal example for obj_reader repair mode.
//
// Builds a manifold half-edge mesh from a polygon soup whose faces have
// inconsistent winding and disconnected fans at shared vertex numbers. Every
// source face is preserved, and the source maps trace each output slot back
// to the file it came from.

#include <polymesh/Mesh.hh>
#include <polymesh/formats/obj.hh>

#include <iostream>
#include <sstream>

int main()
{
    // two triangles that touch only at source vertex 1 (a "butterfly")
    std::istringstream obj("v 0 0 0\n"
                           "v 1 0 0\n"
                           "v 0 1 0\n"
                           "v 2 0 0\n"
                           "v 2 1 0\n"
                           "f 1 2 3\n"
                           "f 1 4 5\n");

    polymesh::Mesh mesh;
    polymesh::obj_reader<double> reader(obj, mesh, polymesh::obj_read_mode::repair);

    // no face is dropped in repair mode
    if (reader.error_faces() != 0)
        return 1;
    mesh.assert_consistency();

    // the shared source position was duplicated, one copy per face fan
    std::cout << "vertices=" << mesh.vertices().size() << " faces=" << mesh.faces().size() << "\n";
    for (auto f : mesh.faces())
        std::cout << "output face " << int(f) << " comes from source face " << reader.face_source()[int(f)] << "\n";

    // the result is a normal editable half-edge mesh
    mesh.edges().split(*mesh.edges().begin());
    mesh.assert_consistency();
    return 0;
}
