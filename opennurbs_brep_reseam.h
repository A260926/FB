#ifndef OPENNURBS_BREP_RESEAM_H
#define OPENNURBS_BREP_RESEAM_H

#if defined(ON_COMPILING_OPENNURBS)
#include "opennurbs.h"
#else
#include "opennurbs_public.h"
#endif

#include <vector>

class ON_TextLog;

// Index maps produced by ON_ReseamBrepFace().
//
// edge_images[i] and trim_images[i] list the result topology pieces made
// from source edge/trim i.  A source element can produce more than one
// result element when a transverse contour is split at the new seam.
// The lists are ordered by increasing parameter on the source element.
// Old seam edges and trims are removed and therefore have empty images.
struct ON_ReseamMap
{
  std::vector<std::vector<int>> edge_images;
  std::vector<std::vector<int>> trim_images;

  // Result indices of the new seam edges, ordered along the open axis.
  std::vector<int> seam_edges;
};

// Move the periodic-direction seam of one face without splitting the face.
// On failure, source, result, and map are left unchanged.  result must not
// alias source.
ON_DECL
bool ON_ReseamBrepFace(
  const ON_Brep& source,
  int face,
  int dir,
  double seam,
  ON_Brep& result,
  ON_ReseamMap& map,
  ON_TextLog* log = nullptr
  );

#endif
