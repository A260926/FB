#pragma once

#include <array>
#include <vector>

#include <polymesh/Mesh.hh>

namespace polymesh
{
/**
 * Extrudes a whole region of faces along a fixed displacement.
 *
 * Input requirements (violations throw std::invalid_argument, leaving mesh AND attributes untouched):
 *  - `region` handles belong to `m`, are unique, valid, and not marked for removal
 *  - `delta` is finite and not the zero vector
 *  - the selected faces at every selected vertex form a single contiguous face fan
 *  - the region boundary consists of disjoint simple closed loops that do not touch each other
 *    and do not lie on the mesh boundary
 *  - `delta` is not parallel to any region boundary edge (that would create zero-area walls)
 *
 * A region may be disconnected and may contain holes.
 *
 * Topology:
 *  - every region boundary vertex is duplicated into the cap; interior region vertices are simply translated
 *  - the selected faces become caps with the same winding (their handles are reused)
 *  - one quad wall is created per directed region boundary half-edge. Its bottom edge is shared with the
 *    previously unselected face, its top edge with the cap. Hole walls follow the boundary's winding.
 *  - no unselected vertex/edge/face/half-edge handle is invalidated
 *
 * Attributes:
 *  - cap vertices, cap edges, cap faces, and cap corners inherit the corresponding original slots
 *    (this is done for every registered attribute, including user-owned non-trivial ones)
 *  - wall faces inherit the adjacent original selected face
 *  - cap top edges inherit the original region boundary edge
 *  - vertical edges and the remaining wall corners keep their default values
 *  - `tex_coord` and `normal` are corner attributes (indexed by the half-edge's vertex_to):
 *      - cap and unselected corners are left untouched
 *      - wall corners receive (u, 0/|delta|, 0) with u accumulated from the smallest-indexed
 *        boundary half-edge of their wall loop (one parameterization per loop / hole)
 *      - wall corner normals are the unit normal of their oriented wall quad
 *
 * The returned vector contains the cap faces (in the order of `region`) and can be used for
 * subsequent edits.
 */
std::vector<face_handle> extrude_region(Mesh& m, //
                                        vertex_attribute<std::array<double, 4>>& position, //
                                        halfedge_attribute<std::array<double, 3>>& tex_coord, //
                                        halfedge_attribute<std::array<double, 3>>& normal, //
                                        std::vector<face_handle> const& region, //
                                        std::array<double, 3> delta);
} // namespace polymesh
