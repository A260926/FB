//=============================================================================
// boundary_bridge.cc
//
// Example: connect the two boundary loops of an open tube with an optimal
// triangle strip using OpenMesh::Utils::bridge_boundaries().
//
// Build (standalone, from the repository root):
//
//   c++ -std=c++11 -DOM_STATIC_BUILD -I. -Isrc \
//       src/OpenMesh/Examples/boundary_bridge.cc \
//       <path-to-libOpenMeshCore.a> <path-to-libOpenMeshTools.a> \
//       -o boundary_bridge
//
// The example needs neither a window nor a GPU.
//=============================================================================

#include <OpenMesh/Core/IO/MeshIO.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Utils/BoundaryBridgeT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

typedef OpenMesh::TriMesh_ArrayKernelT<>  MyMesh;

static const double PI = 3.14159265358979323846;

//------------------------------------------------------------------------------
// Build two "caps": each is a triangle fan around a centre vertex with an
// open polygonal rim.  The two rims are complete boundary loops; the fans
// are oriented so that the rims run in opposite directions, which is what a
// bridge needs.  _n0 and _n1 may differ.
//------------------------------------------------------------------------------

static bool add_cap(MyMesh& _mesh, int _n, float _radius, float _z,
                    std::vector<MyMesh::VertexHandle>& _rim)
{
  std::vector<MyMesh::VertexHandle> rim;
  for (int i = 0; i < _n; ++i)
  {
    const double a = 2.0 * PI * double(i) / double(_n);
    rim.push_back(_mesh.add_vertex(MyMesh::Point(float(_radius * std::cos(a)),
                                                float(_radius * std::sin(a)), _z)));
  }

  const MyMesh::VertexHandle centre = _mesh.add_vertex(MyMesh::Point(0.0f, 0.0f, _z));
  for (int i = 0; i < _n; ++i)
  {
    const int j = (i + 1) % _n;
    // The two caps are given opposite winding so their rims face each other.
    const MyMesh::FaceHandle fh = (_z <= 0.0f)
        ? _mesh.add_face(centre, rim[size_t(j)], rim[size_t(i)])
        : _mesh.add_face(centre, rim[size_t(i)], rim[size_t(j)]);
    if (!fh.is_valid()) return false;
  }

  _rim = rim;
  return true;
}

// Iterate a boundary loop in the direction of the boundary halfedges' next
// pointers.  This is exactly the ordering bridge_boundaries() expects.
static std::vector<MyMesh::VertexHandle>
boundary_loop(MyMesh& _mesh, MyMesh::VertexHandle _start)
{
  MyMesh::HalfedgeHandle h;
  for (MyMesh::ConstVertexOHalfedgeIter it = _mesh.cvoh_iter(_start); it.is_valid(); ++it)
    if (_mesh.is_boundary(*it)) { h = *it; break; }

  std::vector<MyMesh::VertexHandle> loop;
  if (!h.is_valid()) return loop;

  MyMesh::HalfedgeHandle cur = h;
  do
  {
    loop.push_back(_mesh.from_vertex_handle(cur));
    cur = _mesh.next_halfedge_handle(cur);
  }
  while (cur != h && loop.size() < 1000);

  return loop;
}

int main(int argc, char** argv)
{
  const int    n0     = (argc > 1) ? std::atoi(argv[1]) : 6;
  const int    n1     = (argc > 2) ? std::atoi(argv[2]) : 5;
  const double lambda = (argc > 3) ? std::atof(argv[3]) : 1.0;

  if (n0 < 3 || n0 > 32 || n1 < 3 || n1 > 32 || !std::isfinite(lambda) || lambda < 0.0)
  {
    std::cerr << "usage: " << argv[0]
              << " [loop-size-0 3..32] [loop-size-1 3..32] [lambda >= 0]\n";
    return 1;
  }

  MyMesh mesh;
  std::vector<MyMesh::VertexHandle> rim0_raw, rim1_raw;
  if (!add_cap(mesh, n0, 1.0f, 0.0f, rim0_raw) ||
      !add_cap(mesh, n1, 1.5f, 2.0f, rim1_raw))
  {
    std::cerr << "Could not build the input mesh\n";
    return 1;
  }

  const std::vector<MyMesh::VertexHandle> first  = boundary_loop(mesh, rim0_raw[0]);
  const std::vector<MyMesh::VertexHandle> second = boundary_loop(mesh, rim1_raw[0]);

  if (first.size() != size_t(n0) || second.size() != size_t(n1))
  {
    std::cerr << "Unexpected boundary loops\n";
    return 1;
  }

  try
  {
    OpenMesh::Utils::BoundaryBridgeResultT<MyMesh> result;
    OpenMesh::Utils::bridge_boundaries(mesh, first, second, lambda, result);

    std::cout << "Bridged " << result.faces.size() << " triangles, energy = "
              << result.energy << "\n";

    if (!OpenMesh::Utils::MeshCheckerT<MyMesh>(mesh).check())
    {
      std::cerr << "Mesh integrity check failed\n";
      return 1;
    }
  }
  catch (const std::invalid_argument& e)
  {
    std::cerr << "bridge_boundaries: " << e.what() << "\n";
    return 1;
  }

  // Persist the standard halfedge texture coordinates (face corners).
  OpenMesh::IO::Options wopt(OpenMesh::IO::Options::Binary);
  wopt += OpenMesh::IO::Options::FaceTexCoord;
  if (!OpenMesh::IO::write_mesh(mesh, "boundary_bridge.om", wopt))
  {
    std::cerr << "Could not write boundary_bridge.om\n";
    return 1;
  }

  std::cout << "Wrote boundary_bridge.om\n";
  return 0;
}
