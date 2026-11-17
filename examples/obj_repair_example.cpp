// Minimal example for obj_reader's repair mode.
//
// The legacy constructors still add faces line by line and skip conflicting
// faces. Passing obj_read_mode::repair instead analyzes the whole file first:
// winding is made consistent, vertices with disconnected face fans are split,
// every source face is preserved, and invalid input throws std::invalid_argument.
//
// This file is documentation only and is not part of the built library.

#include <fstream>
#include <iostream>
#include <stdexcept>

#include <polymesh/Mesh.hh>
#include <polymesh/formats/obj.hh>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << argv[0] << " model.obj\n";
        return 1;
    }

    polymesh::Mesh mesh;
    try
    {
        polymesh::obj_reader<float> reader(argv[1], mesh, polymesh::obj_read_mode::repair);

        mesh.assert_consistency();

        // repair never silently drops a face
        std::cout << "faces=" << mesh.faces().size() << " vertices=" << mesh.vertices().size() << "\n";

        // trace an output half-edge back to the source file
        auto const& halfedge_source = reader.halfedge_source();
        for (auto h : mesh.halfedges())
        {
            auto const source_corner = halfedge_source[int(h)];
            if (source_corner >= 0)
                std::cout << "half-edge " << int(h) << " came from source corner " << source_corner << "\n";
        }

        // the resulting mesh is a regular half-edge mesh and stays editable
        auto edge = *mesh.edges().begin();
        mesh.edges().split(edge);
        mesh.assert_consistency();
    }
    catch (std::invalid_argument const& e)
    {
        std::cerr << "obj could not be repaired: " << e.what() << "\n";
        return 2;
    }

    return 0;
}
