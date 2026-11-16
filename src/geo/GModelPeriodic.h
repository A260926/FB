// Gmsh - Copyright (C) 1997-2026 C. Geuzaine, J.-F. Remacle
//
// See the LICENSE.txt file in the Gmsh root directory for license information.
// Please report all issues on https://gitlab.onelab.info/gmsh/gmsh/issues.

#ifndef GMODELPERIODIC_H
#define GMODELPERIODIC_H

#include <map>
#include <unordered_set>
#include <vector>

class GEntity;
class GModel;
class MElement;
class MVertex;

// Utilities shared by the periodic mesh readers, the mesh order changes and
// the mesh generator.
//
// The periodic correspondence of an entity keeps the nodes of its slave side
// in two disjoint maps:
//  - correspondingVertices: the nodes that are corners of the linear skeleton
//    of the current elements (the "primary" nodes);
//  - correspondingHighOrderVertices: the other nodes used by the elements.
// Which map a node belongs to follows from the elements that actually use it,
// never from its tag, its coordinates or the polynomial order stored in the
// MVertex. The helpers below implement that classification and reconstruct
// the correspondence from the known corner relations and the local node
// positions of the reference line/triangle/quadrangle (so that reversed edges,
// cyclic face rotations and flips are handled without looking at coordinates).
namespace gmshPeriodic {

// The nodes used by at least one element, and among them the ones used as a
// primary (corner) vertex of at least one element.
void getRoles(GModel *model, std::unordered_set<MVertex *> &used,
              std::unordered_set<MVertex *> &primary);
void getUsedNodes(GModel *model, std::unordered_set<MVertex *> &used);

// Remove the given nodes from all the periodic correspondence maps.
void removeFromMaps(GModel *model,
                    const std::unordered_set<MVertex *> &dead);

// Rebuild the correspondence of every periodic entity from its known corner
// correspondence and the local positions of the nodes of its elements.
bool rebuildAll(GModel *model);

// The permutation of the local node indices of two elements of the same type
// whose corners correspond through 'cornerPairs': the reference position of
// slave node i is the reference position of master node perm[i].
bool localNodePermutation(MElement *slave, MElement *master,
                          const std::map<MVertex *, MVertex *> &cornerPairs,
                          std::vector<int> &perm);

} // namespace gmshPeriodic

#endif
