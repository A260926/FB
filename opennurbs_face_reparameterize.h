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

#if !defined(OPENNURBS_FACE_REPARAMETERIZE_INC_)
#define OPENNURBS_FACE_REPARAMETERIZE_INC_

class ON_Brep;
class ON_TextLog;

/*
Description:
  Exactly reparameterizes a single face of a brep by redistributing
  parameter density toward one end of each of the surface's two
  parameter directions.

  The surface keeps its (possibly non unit) valid parameter intervals
  and its 3d locus.  Writing U = [u0,u1], V = [v0,v1] for the two
  surface domains and

        f_c(t) = c*t / ( 1 + (c-1)*t )                     (0 <= t <= 1)

  for the normalized reparameterization, on success the face's new
  surface satisfies

        S_new( U(x), V(y) ) = S_old( U(f_a(x)), V(f_b(y)) ).

  f_c(0)=0, f_c(1)=1, f_c is strictly increasing for c > 0, f_1 is
  the identity, and f_c and f_{1/c} are inverse maps.  Therefore a
  second call with 1/a and 1/b restores the original geometry and
  parameter mapping (control point counts, knot bookkeeping and
  tolerance values are not required to be restored bit for bit).

  The 2d trimming curves are reparameterized with the inverse maps so
  that, at every trim parameter t, the new 2d point evaluates on the
  new surface to exactly the same 3d boundary point as before.  Trim
  parameter domains, proxy orientation, m_bRev3d and edge/vertex
  associations are never changed.

Parameters:
  brep - [in/out] brep containing the face.  Geometry shared with other
    faces or trims is duplicated before modification, so every other
    component keeps its exact geometry.  Face, loop, trim, edge and
    vertex indices never change, and 3d edge curves are never modified.
  face_index - [in] index of the single face to reparameterize.
  a - [in] positive finite reparameterization constant for the first
    (u) parameter direction.
  b - [in] positive finite reparameterization constant for the second
    (v) parameter direction.
  log - [in] optional log that receives the reason when false is
    returned.

Returns:
  true if successful.  When false is returned the brep is left
  unchanged and still valid; when a log is supplied it describes why
  the request was rejected.

Supported input:
  * a and b must be positive and finite.  a = b = 1 succeeds and
    leaves every value unchanged.
  * The face surface must be an ON_NurbsSurface that is clamped and
    non periodic in both directions, has degree 1, 2 or 3 in each
    direction, and has no singular side.  Non unit domains, non
    uniform knot vectors and positive weight rational surfaces are
    supported.  An approximate refit surface is never substituted:
    the new surface is an exact rational NURBS representation.
  * Every 2d trimming curve of the face must be an ON_NurbsCurve of
    dimension 2, degree 1 to 3, with positive weights and with all
    control points inside the original surface parameter rectangle.
    Because the two trim coordinates carry distinct Mobius denominators,
    the exact reparameterized trim is a rational curve whose degree is
    twice the input degree.  Curves produced by earlier successful calls
    are tagged and stay accepted as inputs on later calls even when their
    degree is above the 1..3 as-authored range; repeated calls keep
    doubling the trim degree, so a reparameterized trim degree cap of 24
    is enforced.  Every other curve type is rejected.
  * Unsupported surface or trim types, invalid indices, bad
    coefficients and non finite results make the function return false
    before the brep is modified.
*/
ON_DECL
bool ON_ReparameterizeBrepFace(
  ON_Brep& brep,
  int face_index,
  double a,
  double b,
  ON_TextLog* log = nullptr
  );

#endif
