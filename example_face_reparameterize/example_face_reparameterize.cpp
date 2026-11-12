/* $NoKeywords: $ */
/*
//
// Copyright (c) 1993-2026 Robert McNeel & Associates. All rights reserved.
// OpenNURBS, Rhinoceros, and Rhino3D are registered trademarks of Robert
// McNeel & Associates.
//
// THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY.
// ALL IMPLIED WARRANTIES OF FITNESS FOR ANY PARTICULAR PURPOSE AND OF
// MERCHANTABILITY ARE HEREBY DISCLAIMED.
//
// For complete openNURBS copyright information see <http://www.opennurbs.org>.
//
////////////////////////////////////////////////////////////////
*/

#include "../opennurbs_public_examples.h"
#include "../opennurbs_face_reparameterize.h"

// This example shows how to redistribute the parameter density of a
// single ON_Brep face with ON_ReparameterizeBrepFace().
//
// The face keeps the same three dimensional shape and the same parameter
// intervals; only the mapping from normalized parameters (x,y) to the
// surface parameters changes through
//
//   f_c(t) = c*t / (1 + (c-1)*t).
//
// The 2d trimming curves are remapped with the inverse function so the
// trimmed 3d boundary is unchanged.  Coefficients a and b must be
// positive finite numbers; a = b = 1 is the identity and changes nothing,
// and applying (1/a,1/b) after (a,b) restores the original parameter
// distribution.

static bool ExampleFaceReparameterize()
{
  // Build a simple rational clamped NURBS surface with one face.
  // degree 2 (order 3), 3 control points per direction, 4 knots:
  // the two end knots each have degree (=2) multiplicity.
  ON_NurbsSurface* surface = new ON_NurbsSurface(3, true, 3, 3, 3, 3);
  const double knot[4] = {0.0, 0.0, 1.0, 1.0};
  for (int k = 0; k < 4; ++k)
  {
    surface->SetKnot(0, k, knot[k]);
    surface->SetKnot(1, k, knot[k]);
  }
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      const double w = 1.0 + 0.1 * i; // strictly positive weight
      surface->SetCV(i, j,
        ON_4dPoint(w * i, w * j, w * (0.2 * i * j), w));
    }
  }
  surface->SetDomain(0, 0.0, 5.0);
  surface->SetDomain(1, -1.0, 2.0);

  ON_Brep* brep = ON_Brep::New();
  ON_BrepFace* face = brep->NewFace(*surface);
  delete surface;
  if ( nullptr == face )
  {
    delete brep;
    return false;
  }
  const int face_index = face->m_face_index;
  if ( !brep->IsValid() )
  {
    delete brep;
    return false;
  }

  // Concentrate parameters toward the low parameter end in u and spread
  // them toward the high parameter end in v.
  const double a = 2.0;
  const double b = 0.5;
  ON_TextLog log;
  if ( !ON_ReparameterizeBrepFace(*brep, face_index, a, b, &log) )
  {
    delete brep;
    return false;
  }
  if ( !brep->IsValid() )
  {
    delete brep;
    return false;
  }

  // The parameter intervals are preserved while the surface evaluation
  // now follows the reparameterized mapping.
  const ON_Surface* reparameterized = brep->m_F[face_index].SurfaceOf();
  const bool domain_preserved =
    reparameterized->Domain(0) == ON_Interval(0.0, 5.0) &&
    reparameterized->Domain(1) == ON_Interval(-1.0, 2.0);

  delete brep;
  return domain_preserved;
}

//
// INTRODUCES VIRTUAL:
//
int main()
{
  ON::Begin();
  const bool ok = ExampleFaceReparameterize();
  ON::End();
  return ok ? 0 : 1;
}
