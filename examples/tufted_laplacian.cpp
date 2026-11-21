/****************************************************************************
* VCGLib tufted Laplacian example.
*
* Builds a triangle mesh with a non manifold edge (three triangles share the
* same geometric edge) and computes a harmonic scalar field on it through the
* production TuftedOperator entry point of vcg::tri::Harmonic. The printed
* values live on the real vertices of the source mesh.
****************************************************************************/

#include <array>
#include <iostream>
#include <vector>

#include <vcg/complex/complex.h>
#include <vcg/complex/algorithms/harmonic.h>
#include <vcg/complex/algorithms/tufted_laplacian.h>
#include <vcg/complex/algorithms/update/topology.h>

class ExVertex;
class ExFace;
struct ExTypes : vcg::UsedTypes<
	vcg::Use<ExVertex>::AsVertexType,
	vcg::Use<ExFace>::AsFaceType> {};
class ExVertex : public vcg::Vertex<ExTypes,
	vcg::vertex::Coord3d, vcg::vertex::BitFlags> {};
class ExFace : public vcg::Face<ExTypes,
	vcg::face::VertexRef, vcg::face::FFAdj, vcg::face::BitFlags> {};
class ExMesh : public vcg::tri::TriMesh<std::vector<ExVertex>, std::vector<ExFace> > {};

static void buildMesh(ExMesh& m,
                      const std::vector<vcg::Point3d>& points,
                      const std::vector<std::array<int, 3> >& faces)
{
	vcg::tri::Allocator<ExMesh>::AddVertices(m, points.size());
	vcg::tri::Allocator<ExMesh>::AddFaces(m, faces.size());
	for (size_t i = 0; i < points.size(); ++i)
		m.vert[i].P() = points[i];
	for (size_t i = 0; i < faces.size(); ++i)
		for (int k = 0; k < 3; ++k)
			m.face[i].V(k) = &m.vert[faces[i][k]];
	vcg::tri::UpdateTopology<ExMesh>::FaceFace(m);
}

int main()
{
	// Three triangles share the edge (v0, v1): a classic non manifold joint.
	// The tips lie on different sides, so an ordinary face-face walk cannot
	// pick "the" other triangle.
	ExMesh mesh;
	buildMesh(mesh,
	          { {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0},
	            {0.5, 1.0, 0.0}, {0.5, 0.0, 1.0}, {0.5, -1.0, 0.0} },
	          { {0, 1, 2}, {0, 1, 3}, {0, 1, 4} });

	// The operator owns its tufted cover; the source mesh is only read.
	vcg::tri::TuftedOperator<ExMesh> op(mesh);

	using H = vcg::tri::Harmonic<ExMesh, double>;
	H::ConstraintVec constraints;
	constraints.push_back({ &mesh.vert[0], 0.0 });
	constraints.push_back({ &mesh.vert[1], 1.0 });

	auto field = vcg::tri::Allocator<ExMesh>::GetPerVertexAttribute<double>(mesh, "field");
	if (!H::ComputeScalarField(mesh, constraints, field, op))
	{
		std::cerr << "harmonic solve failed\n";
		return 1;
	}

	std::cout << "tufted harmonic field on a non manifold edge\n";
	std::cout << "intrinsic flips performed: " << op.flipCount() << "\n";
	std::cout << "minimum edge weight: " << op.minEdgeWeight() << "\n";
	for (size_t i = 0; i < mesh.vert.size(); ++i)
		std::cout << "vertex " << i << " value " << field[i] << "\n";
	return 0;
}
