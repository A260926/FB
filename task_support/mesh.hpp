#ifndef UDIM_BASE_MESH_HPP
#define UDIM_BASE_MESH_HPP
#include <vcg/complex/complex.h>
class UVertex; class UFace;
struct UTypes: vcg::UsedTypes<vcg::Use<UVertex>::AsVertexType,vcg::Use<UFace>::AsFaceType>{};
class UVertex: public vcg::Vertex<UTypes,vcg::vertex::Coord3d,vcg::vertex::Normal3d,vcg::vertex::Qualityd,vcg::vertex::Color4b,vcg::vertex::BitFlags>{};
class UFace: public vcg::Face<UTypes,vcg::face::VertexRef,vcg::face::FFAdj,vcg::face::Normal3d,vcg::face::WedgeTexCoord2f,vcg::face::BitFlags>{};
class UMesh: public vcg::tri::TriMesh<std::vector<UVertex>,std::vector<UFace>>{};
#endif
