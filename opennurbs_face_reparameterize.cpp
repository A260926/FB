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

#include "opennurbs.h"
#include "opennurbs_face_reparameterize.h"

#if !defined(ON_COMPILING_OPENNURBS)
#error ON_COMPILING_OPENNURBS must be defined when compiling opennurbs
#endif

// This file implements an exact, rational reparameterization of one
// ON_BrepFace.  Two different rational maps are involved and they must
// not be confused:
//
// 1. The surface is reparameterized in its own parameters.  For a
//    clamped NURBS direction this is the Lee/Lucian Mobius
//    reparameterization.  The knots move through
//    f_c(t) = c*t/(1+(c-1)*t) and the homogeneous control points pick
//    up positive factors.  The parameter interval is preserved because
//    f_c(0)=0 and f_c(1)=1.  The surface degree never changes.
//
// 2. A 2d trimming curve keeps its own curve parameter t and only the
//    (u,v) point it reports is moved.  Each normalized coordinate goes
//    through the inverse g_c = f_c^{-1}.  The u and v coordinate of a
//    2d curve carry different denominators, so after splitting the
//    trim into Bezier spans the two coordinate polynomials are rebuilt
//    against their common denominator.  That common denominator is the
//    product of two degree-d polynomials, hence the exact result is a
//    degree-2d rational curve.  A single generic 2d projective matrix
//    cannot express this coordinate-wise composition, which is why the
//    explicit Bernstein construction below is required.  The result is
//    always an exact rational curve, never a polyline or a refit.

namespace {

// f_c(t) = c*t/(1+(c-1)*t), t in [0,1].
static double ForwardMobius(double c, double t)
{
  return (c * t) / (1.0 + (c - 1.0) * t);
}

static bool PositiveFinite(double c)
{
  return ON_IsValid(c) && ON_IS_FINITE(c) && c > 0.0;
}

// Positive factor c-(c-1)*x for x in [0,1] and c > 0.  It is the
// denominator of g_c = f_c^{-1} and stays strictly positive on the
// whole unit interval (it equals c at x=0 and 1 at x=1).
static double MobiusDenominator(double c, double x)
{
  return c - (c - 1.0) * x;
}

// Marks a 2d trimming curve produced by this reparameterization so that
// a later call accepts the raised (degree 2d) curve even though it no
// longer satisfies the degree 1..3 input range of an "as authored"
// trim.  The marker is never written to archives.
class ReparameterizedTrimMark : public ON_UserData
{
  ON_OBJECT_DECLARE(ReparameterizedTrimMark);

public:
  ReparameterizedTrimMark()
  {
    m_userdata_uuid = ON_CLASS_ID(ReparameterizedTrimMark);
    m_application_uuid = ON_nil_uuid;
    m_userdata_copycount = 1;
  }
  bool Archive() const override { return false; }
  bool Write(ON_BinaryArchive&) const override { return true; }
  bool Read(ON_BinaryArchive&) override { return true; }
  bool GetDescription(ON_wString& s) override
  {
    s = L"Exact face reparameterization trim marker";
    return true;
  }

  static const ReparameterizedTrimMark* From(const ON_Object* obj)
  {
    return (nullptr == obj) ? nullptr
      : static_cast<const ReparameterizedTrimMark*>(
          obj->GetUserData(ON_CLASS_ID(ReparameterizedTrimMark)));
  }
};

ON_OBJECT_IMPLEMENT(ReparameterizedTrimMark, ON_UserData,
  "7C3E2A41-9E62-4F88-8A2B-1D5F7C0A9E11");

// ---------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------

// Apply one Mobius reparameterization to one direction of a rational
// NURBS surface.  This is the tensor product form of the published
// ON_ReparameterizeRationalNurbsCurve() algorithm; the surface stores
// its control net with two explicit strides instead of one.
static bool ReparameterizeNurbsSurfaceDirection(
  ON_NurbsSurface& srf,
  int dir,
  double c
  )
{
  if ( 1.0 == c )
    return true;
  if ( !srf.MakeRational() )
    return false;

  const int degree = srf.Degree(dir);
  const int cv_count = srf.CVCount(dir);
  if ( degree < 1 || cv_count <= degree )
    return false;

  double* knot = srf.m_knot[dir];
  if ( nullptr == knot )
    return false;

  const double k0 = knot[degree - 1];
  const double k1 = knot[cv_count];
  if ( !ON_IsValid(k0) || !ON_IsValid(k1) || k1 <= k0 )
    return false;
  const double inv_length = 1.0 / (k1 - k0);
  const int knot_count = srf.KnotCount(dir);

  // Normalize to [0,1], run the knots through f_c, keep them normalized
  // for the control point factors, then map back to the unchanged domain.
  for ( int ki = 0; ki < knot_count; ++ki )
  {
    const double kn = (knot[ki] - k0) * inv_length;
    knot[ki] = ForwardMobius(c, kn);
    if ( !ON_IsValid(knot[ki]) )
      return false;
  }

  ON_SimpleArray<double> factor(cv_count);
  factor.SetCount(cv_count);
  for ( int i = 0; i < cv_count; ++i )
  {
    // Lee/Lucian factor: a product of degree Mobius denominators over
    // the knots knot[i] .. knot[i+degree-1].  This matches the pointer
    // arithmetic in ON_ReparameterizeRationalNurbsCurve().  Every factor
    // is positive because the knots stay in [0,1].
    double d = MobiusDenominator(c, knot[i]);
    for ( int j = 1; j < degree; ++j )
      d *= MobiusDenominator(c, knot[i + j]);
    if ( !ON_IsValid(d) || !(d > 0.0) )
      return false;
    factor[i] = d;
  }

  for ( int ki = 0; ki < knot_count; ++ki )
    knot[ki] = k0 + (k1 - k0) * knot[ki];

  const int cv_size = srf.CVSize(); // dim+1, surface is rational now
  if ( 0 == dir )
  {
    for ( int i = 0; i < srf.CVCount(0); ++i )
    {
      const double d = factor[i];
      for ( int j = 0; j < srf.CVCount(1); ++j )
      {
        double* cv = srf.CV(i, j);
        for ( int q = 0; q < cv_size; ++q )
        {
          cv[q] *= d;
          if ( !ON_IsValid(cv[q]) )
            return false;
        }
      }
    }
  }
  else
  {
    for ( int j = 0; j < srf.CVCount(1); ++j )
    {
      const double d = factor[j];
      for ( int i = 0; i < srf.CVCount(0); ++i )
      {
        double* cv = srf.CV(i, j);
        for ( int q = 0; q < cv_size; ++q )
        {
          cv[q] *= d;
          if ( !ON_IsValid(cv[q]) )
            return false;
        }
      }
    }
  }

  srf.DestroySurfaceTree();
  return true;
}

// ---------------------------------------------------------------------
// 2d trimming curves
// ---------------------------------------------------------------------

// Standard binomial coefficient "n choose k".  This file cannot use
// ON_BinomialCoefficient() for this because that routine returns
// C(i+j,i) from its two arguments rather than the conventional C(n,k).
// The degrees here are at most 6, so a tiny exact integer table is both
// sufficient and safe.
static long long Choose(int n, int k)
{
  if ( k < 0 || k > n || n < 0 )
    return 0;
  if ( k > n - k )
    k = n - k;
  long long result = 1;
  for ( int i = 0; i < k; ++i )
    result = result * (n - i) / (i + 1);
  return result;
}

// Multiply two Bernstein polynomials of degree d and return the
// degree-2d Bernstein coefficients.
//   F(t)=sum_i f_i B_i^d, G(t)=sum_j g_j B_j^d
//   h_k = sum_{i+j=k} C(d,i) C(d,k-i) / C(2d,k) * f_i * g_{k-i}
static bool MultiplyBernsteinDegreeD(
  int d,
  const double* f,
  const double* g,
  double* h
  )
{
  for ( int k = 0; k <= 2 * d; ++k )
  {
    double acc = 0.0;
    const int i_lo = (k > d) ? (k - d) : 0;
    const int i_hi = (k < d) ? k : d;
    const double cnk = (double)Choose(2 * d, k);
    if ( !(cnk > 0.0) )
      return false;
    for ( int i = i_lo; i <= i_hi; ++i )
    {
      const double cni = (double)Choose(d, i);
      const double cnm = (double)Choose(d, k - i);
      acc += (cni * cnm / cnk) * f[i] * g[k - i];
    }
    if ( !ON_IsValid(acc) )
      return false;
    h[k] = acc;
  }
  return true;
}

// Build the exact reparameterized 2d curve.  The curve parameter
// domain and span break positions are preserved.  The degree becomes
// 2d because the two coordinate denominators are distinct.
static ON_NurbsCurve* ReparameterizeTrimCurve(
  const ON_NurbsCurve& src,
  const ON_Interval& udom,
  const ON_Interval& vdom,
  double a,
  double b,
  ON_TextLog* log
  )
{
  if ( src.Dimension() != 2 )
  {
    if ( log ) log->Print("Reparameterize: 2d trim curve is not dimension 2.\n");
    return nullptr;
  }

  ON_NurbsCurve* bez = ON_NurbsCurve::Cast(src.DuplicateCurve());
  if ( nullptr == bez )
  {
    if ( log ) log->Print("Reparameterize: unable to duplicate 2d trim curve.\n");
    return nullptr;
  }
  if ( !bez->MakeRational() )
  {
    if ( log ) log->Print("Reparameterize: unable to make 2d trim curve rational.\n");
    delete bez;
    return nullptr;
  }

  const int d = bez->Degree();
  if ( d < 1 )
  {
    if ( log ) log->Print("Reparameterize: bad 2d trim degree.\n");
    delete bez;
    return nullptr;
  }

  const int span_count = bez->SpanCount();
  if ( span_count < 1 )
  {
    if ( log ) log->Print("Reparameterize: 2d trim has no spans.\n");
    delete bez;
    return nullptr;
  }

  ON_SimpleArray<double> span(span_count + 1);
  span.SetCount(span_count + 1);
  if ( !bez->GetSpanVector(span.Array()) )
  {
    if ( log ) log->Print("Reparameterize: unable to read 2d trim span vector.\n");
    delete bez;
    return nullptr;
  }

  // Turn the curve into piecewise Bezier form (every distinct knot,
  // including interior breaks, carries full degree multiplicity) so each
  // span can be reparameterized independently.  MakePiecewiseBezier is
  // the openNURBS routine that handles arbitrary parameter domains.
  if ( !bez->MakePiecewiseBezier(false) )
  {
    if ( log ) log->Print("Reparameterize: unable to split 2d trim into Bezier spans.\n");
    delete bez;
    return nullptr;
  }
  // Re-read the span vector after the knot insertion.
  const int bezier_span_count = bez->SpanCount();
  if ( bezier_span_count != span_count )
  {
    if ( log ) log->Print("Reparameterize: internal error, span count changed during Bezier split.\n");
    delete bez;
    return nullptr;
  }

  const double u0 = udom[0];
  const double u1 = udom[1];
  const double v0 = vdom[0];
  const double v1 = vdom[1];
  const double lu = u1 - u0;
  const double lv = v1 - v0;
  if ( !(lu > 0.0) || !(lv > 0.0) )
  {
    if ( log ) log->Print("Reparameterize: degenerate surface domain for trim mapping.\n");
    delete bez;
    return nullptr;
  }

  // Per-span working arrays.
  ON_SimpleArray<double> X(d + 1), Y(d + 1), W(d + 1);
  ON_SimpleArray<double> A(d + 1), Bp(d + 1), P(d + 1), Q(d + 1);
  X.SetCount(d + 1); Y.SetCount(d + 1); W.SetCount(d + 1);
  A.SetCount(d + 1); Bp.SetCount(d + 1); P.SetCount(d + 1); Q.SetCount(d + 1);
  ON_SimpleArray<double> hU(2 * d + 1), hV(2 * d + 1), hW(2 * d + 1);
  hU.SetCount(2 * d + 1); hV.SetCount(2 * d + 1); hW.SetCount(2 * d + 1);

  const int new_degree = 2 * d;
  // Each exact coordinate-wise composition doubles the Bernstein degree.
  // Bound it so a long chain of calls cannot grow an impractically large
  // curve.  A normal as-authored degree 1..3 trim reaches degree at most
  // 6 after one call and 12 after a second; 24 leaves ample headroom.
  const int kMaxReparameterizedTrimDegree = 24;
  if ( new_degree > kMaxReparameterizedTrimDegree )
  {
    if ( log ) log->Print("Reparameterize: trim degree %d would exceed the supported reparameterized degree %d.\n",
                          new_degree, kMaxReparameterizedTrimDegree);
    delete bez;
    return nullptr;
  }
  const int new_cv_count = span_count * new_degree + 1;
  ON_NurbsCurve* result = new ON_NurbsCurve(2, true, new_degree + 1, new_cv_count);
  if ( !result->m_knot || !result->m_cv )
  {
    if ( log ) log->Print("Reparameterize: unable to allocate reparameterized trim.\n");
    delete bez;
    delete result;
    return nullptr;
  }

  // Piecewise Bezier knot vector.  A NURBS knot vector carries degree
  // (not degree+1) equal knots at a clamped end and degree equal knots
  // at every full multiplicity interior break, for a total of
  // new_degree*(span_count+1) knots.
  int knot_index = 0;
  for ( int s = 0; s <= span_count; ++s )
  {
    for ( int m = 0; m < new_degree; ++m )
      result->SetKnot(knot_index++, span[s]);
  }

  for ( int s = 0; s < span_count; ++s )
  {
    for ( int i = 0; i <= d; ++i )
    {
      const double* cv = bez->CV(s * d + i);
      X[i] = cv[0];
      Y[i] = cv[1];
      W[i] = cv[2];
      if ( !ON_IsValid(X[i]) || !ON_IsValid(Y[i]) || !ON_IsValid(W[i]) || !(W[i] > 0.0) )
      {
        if ( log ) log->Print("Reparameterize: non finite or non positive trim coefficient.\n");
        delete bez;
        delete result;
        return nullptr;
      }

      // Numerator polynomials of the two inverse Mobius denominators
      // and of the two remapped coordinate numerators.  All constants
      // below are derived algebraically so the construction stays exact
      // and every resulting weight remains strictly positive.
      A[i]  = W[i] * (a * lu + (a - 1.0) * u0) - (a - 1.0) * X[i];
      Bp[i] = W[i] * (b * lv + (b - 1.0) * v0) - (b - 1.0) * Y[i];
      P[i]  = (a - 1.0) * u0 * u1 * W[i] + (lu - (a - 1.0) * u0) * X[i];
      Q[i]  = (b - 1.0) * v0 * v1 * W[i] + (lv - (b - 1.0) * v0) * Y[i];
    }

    // U = P * B, V = Q * A, W_new = A * B (all degree 2d).
    if ( !MultiplyBernsteinDegreeD(d, P.Array(), Bp.Array(), hU.Array()) ||
         !MultiplyBernsteinDegreeD(d, Q.Array(), A.Array(),  hV.Array()) ||
         !MultiplyBernsteinDegreeD(d, A.Array(), Bp.Array(), hW.Array()) )
    {
      if ( log ) log->Print("Reparameterize: Bernstein composition failed.\n");
      delete bez;
      delete result;
      return nullptr;
    }

    for ( int k = 0; k <= new_degree; ++k )
    {
      if ( !ON_IsValid(hU[k]) || !ON_IsValid(hV[k]) || !ON_IsValid(hW[k]) || !(hW[k] > 0.0) )
      {
        if ( log ) log->Print("Reparameterize: non finite reparameterized trim coefficient.\n");
        delete bez;
        delete result;
        return nullptr;
      }
      // Adjacent spans share the breakpoint control point, which is
      // written twice with identical values by construction (k=0 of
      // span s+1 equals k=new_degree of span s).
      const int cv_index = s * new_degree + k;
      result->SetCV(cv_index, ON_4dPoint(hU[k], hV[k], 0.0, hW[k]));
    }
  }

  delete bez;

  if ( result->Domain() != src.Domain() )
  {
    if ( log ) log->Print("Reparameterize: internal error, trim domain changed.\n");
    delete result;
    return nullptr;
  }
  result->DestroyCurveTree();
  if ( !result->IsValid() )
  {
    if ( log ) log->Print("Reparameterize: reparameterized trim curve failed validation.\n");
    delete result;
    return nullptr;
  }

  ReparameterizedTrimMark* mark = new ReparameterizedTrimMark();
  if ( !result->AttachUserData(mark) )
    delete mark; // a marker already exists; that is still acceptable

  return result;
}

// ---------------------------------------------------------------------
// Input validation
// ---------------------------------------------------------------------

static bool ValidateInputs(
  const ON_Brep& brep,
  int face_index,
  const ON_NurbsSurface*& nurbs_surface,
  ON_TextLog* log
  )
{
  if ( face_index < 0 || face_index >= brep.m_F.Count() )
  {
    if ( log ) log->Print("Reparameterize: face index %d is out of range.\n", face_index);
    return false;
  }
  const ON_BrepFace& face = brep.m_F[face_index];
  if ( face.m_face_index != face_index )
  {
    if ( log ) log->Print("Reparameterize: face index does not match its slot.\n");
    return false;
  }

  const ON_Surface* srf = face.SurfaceOf();
  if ( nullptr == srf )
  {
    if ( log ) log->Print("Reparameterize: face has no surface.\n");
    return false;
  }
  nurbs_surface = ON_NurbsSurface::Cast(srf);
  if ( nullptr == nurbs_surface )
  {
    if ( log ) log->Print("Reparameterize: face surface is not an ON_NurbsSurface.\n");
    return false;
  }
  if ( nurbs_surface->Dimension() != 3 || !nurbs_surface->IsValid() )
  {
    if ( log ) log->Print("Reparameterize: face surface is not a valid 3d NURBS surface.\n");
    return false;
  }

  for ( int dir = 0; dir < 2; ++dir )
  {
    if ( !nurbs_surface->Domain(dir).IsIncreasing() )
    {
      if ( log ) log->Print("Reparameterize: surface domain %d is not increasing.\n", dir);
      return false;
    }
    const int degree = nurbs_surface->Degree(dir);
    if ( degree < 1 || degree > 3 )
    {
      if ( log ) log->Print("Reparameterize: surface degree %d in direction %d is outside 1..3.\n", degree, dir);
      return false;
    }
    if ( nurbs_surface->IsPeriodic(dir) || !nurbs_surface->IsClamped(dir, 2) )
    {
      if ( log ) log->Print("Reparameterize: surface must be clamped and non periodic in direction %d.\n", dir);
      return false;
    }
  }
  for ( int side = 0; side < 4; ++side )
  {
    if ( nurbs_surface->IsSingular(side) )
    {
      if ( log ) log->Print("Reparameterize: surface has a singular side (%d).\n", side);
      return false;
    }
  }

  if ( nurbs_surface->IsRational() )
  {
    for ( int i = 0; i < nurbs_surface->CVCount(0); ++i )
    {
      for ( int j = 0; j < nurbs_surface->CVCount(1); ++j )
      {
        if ( !PositiveFinite(nurbs_surface->Weight(i, j)) )
        {
          if ( log ) log->Print("Reparameterize: surface has a non positive or non finite weight.\n");
          return false;
        }
      }
    }
  }

  const ON_Interval udom = nurbs_surface->Domain(0);
  const ON_Interval vdom = nurbs_surface->Domain(1);
  const double rect_eps = 1.0e-9 * (1.0 + fabs(udom[0]) + fabs(udom[1])
                                        + fabs(vdom[0]) + fabs(vdom[1]));

  for ( int fli = 0; fli < face.m_li.Count(); ++fli )
  {
    const int li = face.m_li[fli];
    if ( li < 0 || li >= brep.m_L.Count() )
    {
      if ( log ) log->Print("Reparameterize: face references an invalid loop.\n");
      return false;
    }
    const ON_BrepLoop& loop = brep.m_L[li];
    for ( int lti = 0; lti < loop.m_ti.Count(); ++lti )
    {
      const int ti = loop.m_ti[lti];
      if ( ti < 0 || ti >= brep.m_T.Count() )
      {
        if ( log ) log->Print("Reparameterize: loop references an invalid trim.\n");
        return false;
      }
      const ON_Curve* c2 = brep.m_T[ti].TrimCurveOf();
      const ON_NurbsCurve* nc = (nullptr == c2) ? nullptr : ON_NurbsCurve::Cast(c2);
      if ( nullptr == nc )
      {
        if ( log ) log->Print("Reparameterize: trim %d 2d curve is not an ON_NurbsCurve.\n", ti);
        return false;
      }
      if ( !nc->IsValid() || nc->Dimension() != 2 || !nc->Domain().IsIncreasing() )
      {
        if ( log ) log->Print("Reparameterize: trim %d 2d curve is not valid.\n", ti);
        return false;
      }
      if ( nc->IsPeriodic() || !nc->IsClamped(2) )
      {
        if ( log ) log->Print("Reparameterize: trim %d must be clamped and non periodic.\n", ti);
        return false;
      }
      const int degree = nc->Degree();
      // As authored trims are degree 1..3.  A curve above that range is
      // only accepted when this reparameterization produced it.
      if ( degree < 1 || degree > 3 )
      {
        if ( nullptr == ReparameterizedTrimMark::From(nc) )
        {
          if ( log ) log->Print("Reparameterize: trim %d degree %d is outside the supported 1..3 range.\n", ti, degree);
          return false;
        }
      }
      if ( nc->IsRational() )
      {
        for ( int ci = 0; ci < nc->CVCount(); ++ci )
        {
          if ( !PositiveFinite(nc->Weight(ci)) )
          {
            if ( log ) log->Print("Reparameterize: trim %d has a non positive weight.\n", ti);
            return false;
          }
        }
      }
      for ( int ci = 0; ci < nc->CVCount(); ++ci )
      {
        ON_3dPoint eu;
        if ( !nc->GetCV(ci, eu) || !ON_IsValid(eu.x) || !ON_IsValid(eu.y) )
        {
          if ( log ) log->Print("Reparameterize: trim %d has a non finite control point.\n", ti);
          return false;
        }
        if ( eu.x < udom[0] - rect_eps || eu.x > udom[1] + rect_eps ||
             eu.y < vdom[0] - rect_eps || eu.y > vdom[1] + rect_eps )
        {
          if ( log ) log->Print("Reparameterize: trim %d control point lies outside the surface parameter rectangle.\n", ti);
          return false;
        }
      }
    }
  }

  return true;
}

// Index of the face that owns a trim, or -1.
static int FaceOfTrim(const ON_Brep& brep, int trim_index)
{
  if ( trim_index < 0 || trim_index >= brep.m_T.Count() )
    return -1;
  const int li = brep.m_T[trim_index].m_li;
  if ( li < 0 || li >= brep.m_L.Count() )
    return -1;
  return brep.m_L[li].m_fi;
}

// ---------------------------------------------------------------------
// Modification, run first on a deep copy and then on the caller's brep.
// ---------------------------------------------------------------------

static bool ApplyReparameterization(
  ON_Brep& brep,
  int face_index,
  double a,
  double b,
  ON_TextLog* log
  )
{
  ON_BrepFace& face = brep.m_F[face_index];
  const ON_NurbsSurface* old_srf = ON_NurbsSurface::Cast(face.SurfaceOf());
  if ( nullptr == old_srf )
    return false;
  const ON_Interval udom = old_srf->Domain(0);
  const ON_Interval vdom = old_srf->Domain(1);

  // ---- New surface, validated before it is published. ----
  // The Lee/Lucian primitive ON_ReparameterizeRationalNurbsCurve(c) moves
  // the knots through f_c(t) = c*t/(1+(c-1)t), and its external effect is
  // out(t) = in(f_{1/c}(t)) (verified numerically).  The contract needs
  // S_new(x) = S_old(f_a(x)), so passing c = 1/a makes f_{1/c} = f_a and
  // produces exactly the required new surface.  The 2d trims apply the
  // inverse g_a = f_{1/a} to their points, so surface and trims compose
  // back to the original 3d boundary.
  ON_NurbsSurface* new_srf = ON_NurbsSurface::Cast(old_srf->DuplicateSurface());
  if ( nullptr == new_srf )
  {
    if ( log ) log->Print("Reparameterize: unable to duplicate face surface.\n");
    return false;
  }
  if ( !ReparameterizeNurbsSurfaceDirection(*new_srf, 0, 1.0 / a) ||
       !ReparameterizeNurbsSurfaceDirection(*new_srf, 1, 1.0 / b) )
  {
    if ( log ) log->Print("Reparameterize: surface Mobius reparameterization failed.\n");
    delete new_srf;
    return false;
  }
  // The knot rewrite goes through a normalized [0,1] round trip, so the
  // two end knots can differ by a single rounding even though the
  // parameter interval is intended to be unchanged.  Compare the end
  // points against a relative tolerance instead of requiring bit exact
  // equality.
  {
    const ON_Interval d0 = new_srf->Domain(0);
    const ON_Interval d1 = new_srf->Domain(1);
    const double eps0 = 1.0e-12 *
      (1.0 + fabs(udom[0]) + fabs(udom[1]) + fabs(d0[0]) + fabs(d0[1]));
    const double eps1 = 1.0e-12 *
      (1.0 + fabs(vdom[0]) + fabs(vdom[1]) + fabs(d1[0]) + fabs(d1[1]));
    const bool domain_ok =
      fabs(d0[0] - udom[0]) <= eps0 && fabs(d0[1] - udom[1]) <= eps0 &&
      fabs(d1[0] - vdom[0]) <= eps1 && fabs(d1[1] - vdom[1]) <= eps1;
    if ( !domain_ok || !new_srf->IsValid() )
    {
      if ( log ) log->Print("Reparameterize: new surface failed its domain or validity check.\n");
      delete new_srf;
      return false;
    }
  }

  // Numerical surface identity check on an interior grid.
  for ( int ix = 1; ix <= 3; ++ix )
  {
    const double x = ix / 4.0;
    for ( int iy = 1; iy <= 3; ++iy )
    {
      const double y = iy / 4.0;
      const ON_3dPoint p_new = new_srf->PointAt(udom.ParameterAt(x), vdom.ParameterAt(y));
      const ON_3dPoint p_old = old_srf->PointAt(
        udom.ParameterAt(ForwardMobius(a, x)), vdom.ParameterAt(ForwardMobius(b, y)));
      if ( !p_new.IsValid() || !p_old.IsValid() ||
           !ON_IS_FINITE(p_new.x) || !ON_IS_FINITE(p_new.y) || !ON_IS_FINITE(p_new.z) )
      {
        if ( log ) log->Print("Reparameterize: non finite surface evaluation.\n");
        delete new_srf;
        return false;
      }
      const double scale = 1.0
        + fabs(p_old.x) + fabs(p_old.y) + fabs(p_old.z)
        + fabs(p_new.x) + fabs(p_new.y) + fabs(p_new.z);
      if ( p_new.DistanceTo(p_old) > 1.0e-10 * scale )
      {
        if ( log ) log->Print("Reparameterize: surface identity check failed.\n");
        delete new_srf;
        return false;
      }
    }
  }

  // ---- Schedule the distinct 2d curves used by this face. ----
  ON_SimpleArray<int> source_c2;
  for ( int fli = 0; fli < face.m_li.Count(); ++fli )
  {
    const ON_BrepLoop& loop = brep.m_L[face.m_li[fli]];
    for ( int lti = 0; lti < loop.m_ti.Count(); ++lti )
    {
      const int c2i = brep.m_T[loop.m_ti[lti]].m_c2i;
      if ( source_c2.Search(c2i) < 0 )
        source_c2.Append(c2i);
    }
  }

  ON_SimpleArray<bool> shared_outside(source_c2.Count());
  shared_outside.SetCount(source_c2.Count());
  for ( int k = 0; k < source_c2.Count(); ++k )
  {
    bool shared = false;
    for ( int ti = 0; ti < brep.m_T.Count() && !shared; ++ti )
    {
      if ( brep.m_T[ti].m_c2i == source_c2[k] && FaceOfTrim(brep, ti) != face_index )
        shared = true;
    }
    shared_outside[k] = shared;
  }

  // Replacement curves for 2d geometry that only this face references.
  ON_SimpleArray<ON_NurbsCurve*> replacement(source_c2.Count());
  replacement.SetCount(source_c2.Count());
  for ( int k = 0; k < source_c2.Count(); ++k )
  {
    replacement[k] = nullptr;
    if ( shared_outside[k] )
      continue;
    const ON_NurbsCurve* src = ON_NurbsCurve::Cast(brep.m_C2[source_c2[k]]);
    if ( nullptr == src )
    {
      if ( log ) log->Print("Reparameterize: expected a NURBS 2d curve.\n");
      for ( int q = 0; q < k; ++q ) delete replacement[q];
      delete new_srf;
      return false;
    }
    replacement[k] = ReparameterizeTrimCurve(*src, udom, vdom, a, b, log);
    if ( nullptr == replacement[k] )
    {
      for ( int q = 0; q < k; ++q ) delete replacement[q];
      delete new_srf;
      return false;
    }
  }

  // ---- Publish the surface. ----
  if ( brep.SurfaceUseCount(face.m_si, brep.m_F.Count() + 1) > 1 )
  {
    const int new_si = brep.AddSurface(new_srf);
    if ( new_si < 0 )
    {
      if ( log ) log->Print("Reparameterize: unable to add replacement surface.\n");
      for ( int q = 0; q < replacement.Count(); ++q ) delete replacement[q];
      delete new_srf;
      return false;
    }
    face.m_si = new_si;
    face.SetProxySurface(brep.m_S[new_si]);
  }
  else
  {
    ON_NurbsSurface* target = ON_NurbsSurface::Cast(brep.m_S[face.m_si]);
    if ( nullptr == target )
    {
      if ( log ) log->Print("Reparameterize: stored face surface is not a NURBS surface.\n");
      for ( int q = 0; q < replacement.Count(); ++q ) delete replacement[q];
      delete new_srf;
      return false;
    }
    *target = *new_srf;
    delete new_srf;
  }
  face.DestroyRuntimeCache();
  face.DestroyMesh(ON::render_mesh);
  face.DestroyMesh(ON::analysis_mesh);
  face.DestroyMesh(ON::preview_mesh);

  // ---- Publish the 2d curves. ----
  const double tol_u_factor = (a >= 1.0) ? a : 1.0 / a;
  const double tol_v_factor = (b >= 1.0) ? b : 1.0 / b;

  for ( int fli = 0; fli < face.m_li.Count(); ++fli )
  {
    ON_BrepLoop& loop = brep.m_L[face.m_li[fli]];
    loop.DestroyRuntimeCache();
    loop.m_pbox.Destroy();

    for ( int lti = 0; lti < loop.m_ti.Count(); ++lti )
    {
      ON_BrepTrim& trim = brep.m_T[loop.m_ti[lti]];
      const int slot = source_c2.Search(trim.m_c2i);
      if ( slot < 0 )
      {
        if ( log ) log->Print("Reparameterize: internal error, trim curve not scheduled.\n");
        return false;
      }

      trim.DestroyCurveTree();
      trim.m_pline.Destroy();

      if ( shared_outside[slot] )
      {
        // The 2d geometry is shared with another face's trim, so the
        // source curve must stay untouched.  Reparameterize a private
        // copy of the raw curve (its full domain is preserved) and then
        // rebuild this trim's proxy so its sub domain, this domain and
        // reversal flag all keep their original numeric values.
        const ON_NurbsCurve* raw = ON_NurbsCurve::Cast(brep.m_C2[trim.m_c2i]);
        if ( nullptr == raw )
        {
          if ( log ) log->Print("Reparameterize: shared 2d curve is not a NURBS curve.\n");
          return false;
        }
        const bool saved_reversed = trim.ProxyCurveIsReversed();
        const ON_Interval saved_real_domain = trim.ProxyCurveDomain();
        const ON_Interval saved_this_domain = trim.Domain();
        // A reversed proxy stores a decreasing (negated) this domain.
        // Undo that negation to get the increasing domain that is set
        // while the reversal flag is still clear.
        ON_Interval increasing_this = saved_this_domain;
        if ( saved_reversed )
          increasing_this.Set(-saved_this_domain[1], -saved_this_domain[0]);

        ON_NurbsCurve* private_curve =
          ReparameterizeTrimCurve(*raw, udom, vdom, a, b, log);
        if ( nullptr == private_curve )
          return false;
        const int new_c2i = brep.AddTrimCurve(private_curve);
        if ( new_c2i < 0 || !trim.ChangeTrimCurve(new_c2i) )
        {
          if ( log ) log->Print("Reparameterize: unable to attach private trim curve.\n");
          delete private_curve;
          return false;
        }
        // The replacement curve keeps the raw curve's parameter domain,
        // so the saved real-curve sub domain keeps its meaning.
        trim.SetProxyCurveDomain(saved_real_domain);
        trim.SetDomain(increasing_this);
        if ( saved_reversed )
        {
          // Use the base proxy reverse (toggles m_bReversed and negates
          // the this domain), not the ON_BrepTrim override which would
          // also rewrite vertices and m_bRev3d.
          trim.ON_CurveProxy::Reverse();
        }
      }
      else
      {
        // Replace the owned 2d geometry in place.  The target address
        // does not change, so every proxy keeps its pointer, sub domain
        // and reversal flag automatically; only the cached box and tree
        // have to be refreshed.
        ON_NurbsCurve* target = ON_NurbsCurve::Cast(brep.m_C2[trim.m_c2i]);
        if ( nullptr == target )
        {
          if ( log ) log->Print("Reparameterize: stored 2d curve is not a NURBS curve.\n");
          return false;
        }
        *target = *replacement[slot];
        trim.m_pbox = target->BoundingBox();
        trim.m_pbox.m_min.z = 0.0;
        trim.m_pbox.m_max.z = 0.0;
      }

      // Conservative nonnegative parameter tolerances.
      if ( trim.m_tolerance[0] >= 0.0 )
        trim.m_tolerance[0] *= tol_u_factor;
      if ( trim.m_tolerance[1] >= 0.0 )
        trim.m_tolerance[1] *= tol_v_factor;
    }
  }

  for ( int q = 0; q < replacement.Count(); ++q )
    delete replacement[q];

  // Recompute parameter space boxes and iso flags, then drop caches.
  brep.SetTrimBoundingBoxes(face, false);
  brep.SetTrimIsoFlags(face);
  face.DestroyRuntimeCache();

  return true;
}

} // anonymous namespace

bool ON_ReparameterizeBrepFace(
  ON_Brep& brep,
  int face_index,
  double a,
  double b,
  ON_TextLog* log
  )
{
  if ( !PositiveFinite(a) || !PositiveFinite(b) )
  {
    if ( log ) log->Print("Reparameterize: a and b must be positive finite numbers.\n");
    return false;
  }
  if ( face_index < 0 || face_index >= brep.m_F.Count() )
  {
    if ( log ) log->Print("Reparameterize: face index %d is out of range [0,%d).\n",
                          face_index, brep.m_F.Count());
    return false;
  }

  const ON_NurbsSurface* nurbs_surface = nullptr;
  if ( !ValidateInputs(brep, face_index, nurbs_surface, log) )
    return false;

  // Identity reparameterization always succeeds and changes nothing.
  if ( 1.0 == a && 1.0 == b )
    return true;

  // Dry run on a deep copy so the caller's brep is only touched once
  // the whole transformation has been built and validated.
  ON_Brep* trial = ON_Brep::New(brep);
  if ( nullptr == trial )
  {
    if ( log ) log->Print("Reparameterize: unable to duplicate brep for validation.\n");
    return false;
  }
  if ( !ApplyReparameterization(*trial, face_index, a, b, log) || !trial->IsValid(log) )
  {
    delete trial;
    if ( log ) log->Print("Reparameterize: validation run failed; brep left unchanged.\n");
    return false;
  }
  delete trial;

  if ( !ApplyReparameterization(brep, face_index, a, b, log) )
    return false;

  if ( !brep.IsValid(log) )
  {
    if ( log ) log->Print("Reparameterize: final brep validation failed.\n");
    return false;
  }
  return true;
}
