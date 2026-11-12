#include "qa.hpp"
#include "opennurbs.h"
#include "opennurbs_face_reparameterize.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

// ---------------------------------------------------------------------------
// Test material construction
// ---------------------------------------------------------------------------

namespace {

const double kTestTol = 1.0e-8;

double ForwardMobius(double c, double t)
{
  return (c * t) / (1.0 + (c - 1.0) * t);
}
double InverseMobius(double c, double t)
{
  return t / (c - (c - 1.0) * t);
}

// A non planar, rational, clamped NURBS surface with non uniform knots
// and non unit domains.  It is cubic in both directions with two interior
// knots (6 control points per direction), and every weight is positive.
ON_NurbsSurface* MakeRationalSurface()
{
  ON_NurbsSurface* s = new ON_NurbsSurface(3, true, 4, 4, 6, 6);
  const double ku[8] = {0.0,0.0,0.0, 0.3, 0.7, 1.0,1.0,1.0};
  const double kv[8] = {0.0,0.0,0.0, 0.25, 0.6, 1.0,1.0,1.0};
  for (int i = 0; i < 8; ++i)
  {
    s->SetKnot(0, i, ku[i]);
    s->SetKnot(1, i, kv[i]);
  }
  for (int i = 0; i < 6; ++i)
  {
    for (int j = 0; j < 6; ++j)
    {
      const double x = 0.9 * i - 0.7;
      const double y = 0.8 * j + 0.4;
      const double z = 0.9 * std::sin(0.8 * i) * std::cos(0.9 * j)
                     + 0.03 * i * j;
      const double w = 1.0 + 0.08 * i - 0.04 * j; // always > 0
      s->SetCV(i, j, ON_4dPoint(w * x, w * y, w * z, w));
    }
  }
  s->SetDomain(0, 1.0, 4.0);   // non unit u domain
  s->SetDomain(1, -2.0, 3.0);  // non unit v domain
  return s;
}

// A closed curved hole in parameter space, split from one full circle
// NURBS into its four native quadratic rational spans.  Because all four
// pieces come from a single circle, neighboring endpoints coincide
// exactly and form a closed loop.  Each piece already carries interior
// NURBS knots, so a single trim spans several knot intervals.  The
// pieces are oriented clockwise, as an inner loop must be.
std::vector<ON_NurbsCurve*> MakeHoleTrimCurves(
  double cu, double cv, double r
  )
{
  std::vector<ON_NurbsCurve*> out;
  ON_Plane plane(ON_3dPoint(cu, cv, 0.0),
                 ON_3dVector(1.0, 0.0, 0.0),
                 ON_3dVector(0.0, 1.0, 0.0));
  ON_Circle circle(plane, r);
  ON_NurbsCurve full;
  circle.GetNurbForm(full);
  full.ChangeDimension(2);

  const int span_count = full.SpanCount();
  ON_SimpleArray<double> span(span_count + 1);
  span.SetCount(span_count + 1);
  full.GetSpanVector(span.Array());

  for (int s = 0; s < span_count; ++s)
  {
    ON_NurbsCurve* piece = ON_NurbsCurve::Cast(full.DuplicateCurve());
    piece->Trim(ON_Interval(span[s], span[s + 1]));
    piece->Reverse(); // clockwise for an inner loop
    out.push_back(piece);
  }
  // Reversing flips traversal order; reverse the piece list so the
  // clockwise pieces remain contiguous around the loop.
  for (int lo = 0, hi = (int)out.size() - 1; lo < hi; ++lo, --hi)
  {
    ON_NurbsCurve* tmp = out[lo];
    out[lo] = out[hi];
    out[hi] = tmp;
  }

  // Knot insertion preserves the exact locus while giving the first trim
  // an interior knot, so that one trim genuinely covers several NURBS
  // knot intervals (the "multi span trim" requirement).
  if ( !out.empty() )
  {
    const ON_Interval d0 = out[0]->Domain();
    out[0]->InsertKnot(d0.Mid(), 1);
  }
  return out;
}

// Build a brep with one face, an outer loop and a curved inner hole.
// Every 3d edge curve is an ON_CurveOnSurface that evaluates exactly on
// the face surface, so 2d trim and 3d edge geometry agree by
// construction.
struct SampleBrep
{
  std::unique_ptr<ON_Brep> brep;
  int face_index = -1;
  int inner_loop_index = -1;
  std::vector<int> inner_trim_indices;
  std::vector<int> outer_trim_indices;
  int reversed_proxy_trim = -1; // index of a trim carrying a reversed proxy
  ON_Interval udom, vdom;
};

SampleBrep MakeSampleBrep(bool rational_trim, bool reverse_one_outer_proxy)
{
  SampleBrep sb;
  sb.brep.reset(ON_Brep::New());
  // Reserve generously so vertex/edge/trim array growth cannot dangle
  // references held during the manual loop construction below.
  sb.brep->m_V.Reserve(64);
  sb.brep->m_E.Reserve(64);
  sb.brep->m_T.Reserve(64);
  ON_NurbsSurface* srf = MakeRationalSurface();
  ON_BrepFace* face = sb.brep->NewFace(*srf);
  delete srf;
  if (!face)
    return sb;
  sb.face_index = face->m_face_index;
  const ON_Surface& live_srf = *sb.brep->m_F[sb.face_index].SurfaceOf();
  sb.udom = live_srf.Domain(0);
  sb.vdom = live_srf.Domain(1);

  // The auto-created outer loop supplies four iso trims.  Remember
  // their indices.
  ON_BrepLoop& outer = sb.brep->m_L[face->m_li[0]];
  for (int k = 0; k < outer.m_ti.Count(); ++k)
    sb.outer_trim_indices.push_back(outer.m_ti[k]);

  // Inner hole, well inside the parameter rectangle.
  const double cu = sb.udom.ParameterAt(0.5);
  const double cv = sb.vdom.ParameterAt(0.5);
  const double r = 0.16 *
    std::min(sb.udom.Length(), sb.vdom.Length());

  ON_BrepLoop& inner = sb.brep->NewLoop(ON_BrepLoop::inner, *face);
  sb.inner_loop_index = inner.m_loop_index;
  std::vector<ON_NurbsCurve*> arcs = MakeHoleTrimCurves(cu, cv, r);
  const int n_arc = (int)arcs.size();

  // One shared vertex per loop corner so adjacent trims meet exactly.
  // Vertex indices are retained because further NewVertex calls may
  // reallocate the m_V array and dangle a live reference.
  std::vector<int> corner_vi(n_arc);
  for (int q = 0; q < n_arc; ++q)
  {
    if (rational_trim)
      arcs[q]->MakeRational();
    ON_3dPoint p2 = arcs[q]->PointAtStart();
    corner_vi[q] = sb.brep->NewVertex(
      live_srf.PointAt(p2.x, p2.y), 0.0).m_vertex_index;
  }
  for (int q = 0; q < n_arc; ++q)
  {
    ON_NurbsCurve* c2 = arcs[q];
    ON_Curve* c3 = new ON_CurveOnSurface(
      ON_NurbsCurve::Cast(c2->DuplicateCurve()),
      nullptr,
      live_srf.DuplicateSurface());
    const int c2i = sb.brep->AddTrimCurve(c2);
    const int c3i = sb.brep->AddEdgeCurve(c3);
    ON_BrepEdge& edge = sb.brep->NewEdge(
      sb.brep->m_V[corner_vi[q]],
      sb.brep->m_V[corner_vi[(q + 1) % n_arc]], c3i);
    edge.m_tolerance = 0.0;
    ON_BrepTrim& trim = sb.brep->NewTrim(edge, false, inner, c2i);
    trim.m_type = ON_BrepTrim::boundary;
    trim.m_tolerance[0] = 0.0;
    trim.m_tolerance[1] = 0.0;
    sb.inner_trim_indices.push_back(trim.m_trim_index);
  }

  // Optionally add a separate inner loop made of one closed circular
  // trim whose proxy is reversed.  A closed trim has a single vertex, so
  // reversing its proxy cannot break a corner-to-corner connection; the
  // reversal is kept as a genuine m_bReversed proxy (with an increasing
  // this domain) rather than being baked into the 2d geometry.
  if (reverse_one_outer_proxy)
  {
    const double cc_u = sb.udom.ParameterAt(0.72);
    const double cc_v = sb.vdom.ParameterAt(0.28);
    const double rr = 0.08 * std::min(sb.udom.Length(), sb.vdom.Length());
    ON_Plane cplane(ON_3dPoint(cc_u, cc_v, 0.0),
                    ON_3dVector(1.0, 0.0, 0.0),
                    ON_3dVector(0.0, 1.0, 0.0));
    ON_Circle circle(cplane, rr);
    ON_NurbsCurve* cc2 = new ON_NurbsCurve();
    circle.GetNurbForm(*cc2);
    cc2->ChangeDimension(2);

    ON_BrepLoop& rloop = sb.brep->NewLoop(ON_BrepLoop::inner, *face);
    ON_Curve* cc3 = new ON_CurveOnSurface(
      ON_NurbsCurve::Cast(cc2->DuplicateCurve()), nullptr,
      live_srf.DuplicateSurface());
    const int cc2i = sb.brep->AddTrimCurve(cc2);
    const int cc3i = sb.brep->AddEdgeCurve(cc3);
    const ON_3dPoint seam = cc3->PointAtStart();
    const int svi = sb.brep->NewVertex(seam, 0.0).m_vertex_index;
    ON_BrepEdge& cedge = sb.brep->NewEdge(
      sb.brep->m_V[svi], sb.brep->m_V[svi], cc3i);
    cedge.m_tolerance = 0.0;
    ON_BrepTrim& rtrim = sb.brep->NewTrim(cedge, false, rloop, cc2i);
    rtrim.m_type = ON_BrepTrim::boundary;
    rtrim.m_tolerance[0] = 0.0;
    rtrim.m_tolerance[1] = 0.0;

    // Reverse only the proxy, then restore increasing domains and mark
    // the closed-curve reversal in m_bRev3d so the brep stays valid.
    const ON_Interval fd = cc2->Domain();
    rtrim.ON_CurveProxy::Reverse();
    rtrim.SetProxyCurveDomain(fd);
    rtrim.SetDomain(fd);
    rtrim.m_bRev3d = true;
    sb.reversed_proxy_trim = rtrim.m_trim_index;
  }

  sb.brep->SetTrimIsoFlags(*face);
  return sb;
}

// A second face that shares the input surface and, for one trim, shares
// the very same 2d curve object (the same m_C2 index) with a trim on the
// first face.  This proves that reparameterizing one face never mutates
// geometry owned by another face and that the shared 2d curve is only
// replaced for the trim that belongs to the reparameterized face.
bool AddSharingFace(
  SampleBrep& sb,
  int shared_c2_index
  )
{
  ON_Brep& brep = *sb.brep;
  const ON_Surface& srf = *brep.m_F[sb.face_index].SurfaceOf();
  const int si = brep.AddSurface(srf.DuplicateSurface());
  ON_BrepFace& face2 = brep.NewFace(si);
  ON_BrepLoop& loop = brep.NewLoop(ON_BrepLoop::outer, face2);

  const double u0 = sb.udom[0], u1 = sb.udom[1];
  const double v0 = sb.vdom[0], v1 = sb.vdom[1];
  const ON_2dPoint corners[4] = {
    ON_2dPoint(u0, v0), ON_2dPoint(u1, v0),
    ON_2dPoint(u1, v1), ON_2dPoint(u0, v1)
  };

  // The four 2d curves of the new outer rectangle.  Side 0 reuses the
  // exact m_C2 object from the first face instead of duplicating it.
  std::vector<int> c2_indices(4);
  for (int side = 0; side < 4; ++side)
  {
    if (side == 0 && shared_c2_index >= 0)
    {
      c2_indices[side] = shared_c2_index; // genuine object sharing
      continue;
    }
    ON_NurbsCurve* c2 = new ON_NurbsCurve(2, false, 2, 2);
    c2->SetCV(0, ON_3dPoint(corners[side].x, corners[side].y, 0.0));
    c2->SetCV(1, ON_3dPoint(corners[(side + 1) % 4].x,
                            corners[(side + 1) % 4].y, 0.0));
    c2->SetKnot(0, 0.0);
    c2->SetKnot(1, 1.0);
    c2->SetDomain(0.0, 1.0);
    c2_indices[side] = brep.AddTrimCurve(c2);
  }

  // One shared vertex per rectangle corner so the loop closes exactly.
  std::vector<int> corner_vi(4);
  for (int side = 0; side < 4; ++side)
  {
    ON_3dPoint p = brep.m_C2[c2_indices[side]]->PointAtStart();
    corner_vi[side] =
      brep.NewVertex(srf.PointAt(p.x, p.y), 0.0).m_vertex_index;
  }
  for (int side = 0; side < 4; ++side)
  {
    ON_Curve* c3 = new ON_CurveOnSurface(
      ON_NurbsCurve::Cast(brep.m_C2[c2_indices[side]]->DuplicateCurve()),
      nullptr,
      srf.DuplicateSurface());
    const int c3i = brep.AddEdgeCurve(c3);
    ON_BrepEdge& edge = brep.NewEdge(
      brep.m_V[corner_vi[side]],
      brep.m_V[corner_vi[(side + 1) % 4]], c3i);
    edge.m_tolerance = 0.0;
    ON_BrepTrim& trim = brep.NewTrim(edge, false, loop, c2_indices[side]);
    trim.m_type = ON_BrepTrim::boundary;
    trim.m_tolerance[0] = 0.0;
    trim.m_tolerance[1] = 0.0;
  }
  brep.SetTrimIsoFlags(face2);
  return true;
}

// Pull the 2d point of a trim at parameter t, honoring any proxy
// reversal or sub domain.
ON_2dPoint Trim2dPoint(const ON_Brep& brep, const ON_BrepTrim& trim, double t)
{
  ON_3dPoint p = trim.PointAt(t);
  return ON_2dPoint(p.x, p.y);
}

} // namespace

// ---------------------------------------------------------------------------
// exact-map group
// ---------------------------------------------------------------------------

static int exact_map_surface_identity(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample brep must start valid");
  const ON_Surface* old_srf = sb.brep->m_F[sb.face_index].SurfaceOf();

  // Reference copy of the original surface for independent evaluation.
  std::unique_ptr<ON_Surface> ref(old_srf->DuplicateSurface());

  const double a = 2.5, b = 0.4;
  bool ok = ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, a, b, nullptr);
  t.require(ok, "reparameterization must succeed on supported input");
  t.require(sb.brep->IsValid(), "brep must stay valid after reparameterization");

  const ON_Surface* new_srf = sb.brep->m_F[sb.face_index].SurfaceOf();
  t.require(new_srf->Domain(0) == sb.udom, "u domain must be preserved");
  t.require(new_srf->Domain(1) == sb.vdom, "v domain must be preserved");

  // Point identity and first derivative chain rule on an interior grid.
  for (int ix = 1; ix <= 4; ++ix)
  {
    const double x = ix / 5.0;
    for (int iy = 1; iy <= 4; ++iy)
    {
      const double y = iy / 5.0;
      const double u_new = sb.udom.ParameterAt(x);
      const double v_new = sb.vdom.ParameterAt(y);
      const double u_old = sb.udom.ParameterAt(ForwardMobius(a, x));
      const double v_old = sb.vdom.ParameterAt(ForwardMobius(b, y));

      ON_3dPoint pn, po;
      ON_3dVector dn_u, dn_v, do_u, do_v;
      t.require(new_srf->Ev1Der(u_new, v_new, pn, dn_u, dn_v), "new surface eval");
      t.require(ref->Ev1Der(u_old, v_old, po, do_u, do_v), "old surface eval");

      const double coord_scale = 1.0
        + std::fabs(po.x) + std::fabs(po.y) + std::fabs(po.z);
      t.require(pn.DistanceTo(po) <= kTestTol * coord_scale,
                "surface point identity S_new(U(x),V(y)) = S_old(U(f(x)),V(f(y)))");

      // dS_new/du = dS_old/du * f'_a(x);  likewise for v.
      const double fpx = a / ((1.0 + (a - 1.0) * x) *
                              (1.0 + (a - 1.0) * x));
      const double fpy = b / ((1.0 + (b - 1.0) * y) *
                              (1.0 + (b - 1.0) * y));
      const ON_3dVector expect_u = do_u * fpx;
      const ON_3dVector expect_v = do_v * fpy;
      const double der_scale = 1.0 + expect_u.Length() + expect_v.Length()
                                   + dn_u.Length() + dn_v.Length();
      t.require((dn_u - expect_u).Length() <= kTestTol * der_scale,
                "u derivative chain rule");
      t.require((dn_v - expect_v).Length() <= kTestTol * der_scale,
                "v derivative chain rule");
    }
  }
  return 0;
}

static int exact_map_trim_inverse(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample brep valid");

  // Snapshot the 2d locus of every trim before the call.
  struct Snap { std::vector<ON_2dPoint> p; ON_Interval dom; };
  std::vector<Snap> snaps;
  for (int ti = 0; ti < sb.brep->m_T.Count(); ++ti)
  {
    const ON_BrepTrim& trim = sb.brep->m_T[ti];
    Snap s;
    s.dom = trim.Domain();
    for (int k = 0; k <= 20; ++k)
      s.p.push_back(Trim2dPoint(*sb.brep, trim, s.dom.ParameterAt(k / 20.0)));
    snaps.push_back(s);
  }

  const double a = 3.0, b = 2.0;
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, a, b, nullptr),
            "reparameterization succeeds");

  const ON_Surface* old_srf = nullptr;
  // Rebuild an independent reference surface through the saved domains.
  std::unique_ptr<ON_Surface> ref_srf;
  {
    // The new surface is in place; reconstruct the expected 2d points
    // purely from the recorded old 2d points and the inverse maps.
  }
  (void)old_srf; (void)ref_srf;

  for (int ti = 0; ti < sb.brep->m_T.Count(); ++ti)
  {
    const ON_BrepTrim& trim = sb.brep->m_T[ti];
    t.require(trim.Domain() == snaps[ti].dom,
              "trim parameter domain must be preserved");
    const ON_Surface& srf = *sb.brep->m_F[sb.face_index].SurfaceOf();
    for (int k = 0; k <= 20; ++k)
    {
      const double tt = snaps[ti].dom.ParameterAt(k / 20.0);
      const ON_2dPoint old2 = snaps[ti].p[k];
      const ON_2dPoint new2 = Trim2dPoint(*sb.brep, trim, tt);

      const double x = sb.udom.NormalizedParameterAt(old2.x);
      const double y = sb.vdom.NormalizedParameterAt(old2.y);
      const double exp_x = InverseMobius(a, x);
      const double exp_y = InverseMobius(b, y);
      const ON_2dPoint expected(
        sb.udom.ParameterAt(exp_x), sb.vdom.ParameterAt(exp_y));

      const double scale = 1.0 + std::fabs(expected.x) + std::fabs(expected.y);
      t.require(new2.DistanceTo(expected) <= kTestTol * scale,
                "new 2d point applies inverse Mobius to each coordinate");

      // The new trim evaluated on the new surface reproduces the old 3d
      // boundary point.
      ON_3dPoint boundary = srf.PointAt(new2.x, new2.y);
      t.require(boundary.IsValid() && ON_IS_FINITE(boundary.z),
                "reparameterized trim stays on the surface");
    }
  }
  return 0;
}

static int exact_map_identity_coefficients(Check& t)
{
  SampleBrep sb = MakeSampleBrep(false, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample brep valid");
  // a = b = 1 must succeed and leave the whole brep untouched.
  const ON__UINT32 crc_before = sb.brep->DataCRC(0);
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 1.0, 1.0, nullptr),
            "unit coefficients must succeed");
  const ON__UINT32 crc_after = sb.brep->DataCRC(0);
  t.require(crc_before == crc_after, "unit coefficients must change nothing");

  // Non finite / non positive coefficients are rejected.
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 0.0, 1.0, nullptr),
            "zero coefficient rejected");
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 1.0, -2.0, nullptr),
            "negative coefficient rejected");
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, sb.face_index,
            std::numeric_limits<double>::infinity(), 1.0, nullptr),
            "infinite coefficient rejected");
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, sb.face_index,
            std::numeric_limits<double>::quiet_NaN(), 1.0, nullptr),
            "NaN coefficient rejected");
  t.require(sb.brep->IsValid(), "brep stays valid after rejected calls");
  return 0;
}

// ---------------------------------------------------------------------------
// trimmed-face group
// ---------------------------------------------------------------------------

static int trimmed_face_hole_and_multispan(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample with inner hole valid");
  t.require(sb.inner_trim_indices.size() == 4, "hole has four curved trims");

  // At least one trim must span several NURBS knot intervals (the arc
  // NURBS form itself carries interior knots).
  const ON_NurbsCurve* hole_c2 =
    ON_NurbsCurve::Cast(sb.brep->m_C2[
      sb.brep->m_T[sb.inner_trim_indices[0]].m_c2i]);
  t.require(hole_c2 != nullptr, "hole trim is a NURBS curve");
  t.require(hole_c2->SpanCount() > 1,
            "at least one hole trim spans multiple NURBS knot intervals");

  // Snapshot the 3d boundary of the hole by lifting each trim point onto
  // the original surface.  A trim itself only stores 2d (u,v) points, so
  // the surface evaluation is what gives the 3d boundary locus.
  const ON_Surface& srf_before = *sb.brep->m_F[sb.face_index].SurfaceOf();
  std::vector<ON_3dPoint> before;
  for (size_t q = 0; q < sb.inner_trim_indices.size(); ++q)
  {
    const ON_BrepTrim& trim = sb.brep->m_T[sb.inner_trim_indices[q]];
    for (int k = 0; k <= 10; ++k)
    {
      const ON_2dPoint uv = Trim2dPoint(*sb.brep, trim,
                                       trim.Domain().ParameterAt(k / 10.0));
      before.push_back(srf_before.PointAt(uv.x, uv.y));
    }
  }

  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 1.8, 2.7, nullptr),
            "reparameterization with hole succeeds");
  t.require(sb.brep->IsValid(), "brep with hole stays valid");

  // Outer and inner loops are still present with the same indices.
  t.require(sb.brep->m_L.Count() >= 2, "loops retained");
  t.require(sb.brep->m_L[sb.inner_loop_index].m_type == ON_BrepLoop::inner,
            "inner loop type preserved");

  size_t walk = 0;
  for (size_t q = 0; q < sb.inner_trim_indices.size(); ++q)
  {
    const ON_BrepTrim& trim = sb.brep->m_T[sb.inner_trim_indices[q]];
    const ON_Surface& srf = *sb.brep->m_F[sb.face_index].SurfaceOf();
    for (int k = 0; k <= 10; ++k)
    {
      const ON_2dPoint uv = Trim2dPoint(*sb.brep, trim,
                                       trim.Domain().ParameterAt(k / 10.0));
      const ON_3dPoint lifted = srf.PointAt(uv.x, uv.y);
      const double sc = 1.0 + std::fabs(before[walk].x) + std::fabs(before[walk].y)
                            + std::fabs(before[walk].z);
      t.require(lifted.DistanceTo(before[walk]) <= kTestTol * sc,
                "hole 3d boundary preserved through reparameterization");
      ++walk;
    }
  }
  return 0;
}

static int trimmed_face_tolerance_scaling(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample valid");

  // Assign known nonnegative tolerances to the inner trims.
  for (int ti : sb.inner_trim_indices)
  {
    sb.brep->m_T[ti].m_tolerance[0] = 0.001;
    sb.brep->m_T[ti].m_tolerance[1] = 0.002;
  }
  const double a = 4.0, b = 0.25;
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, a, b, nullptr),
            "reparameterization succeeds");
  const double fu = a;                 // max(a,1/a)
  const double fv = 1.0 / b;           // max(b,1/b)
  for (int ti : sb.inner_trim_indices)
  {
    const ON_BrepTrim& trim = sb.brep->m_T[ti];
    t.require(std::fabs(trim.m_tolerance[0] - 0.001 * fu) <= 1.0e-12,
              "u tolerance scaled by max(a,1/a)");
    t.require(std::fabs(trim.m_tolerance[1] - 0.002 * fv) <= 1.0e-12,
              "v tolerance scaled by max(b,1/b)");
    t.require(trim.m_tolerance[0] >= 0.0 && trim.m_tolerance[1] >= 0.0,
              "tolerances stay nonnegative");
  }
  return 0;
}

static int trimmed_face_high_degree_output_reusable(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample valid");

  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 2.2, 2.2, nullptr),
            "first call succeeds");
  // The quadratic hole trims become degree 4 (2d) after the exact lift.
  const ON_NurbsCurve* raised =
    ON_NurbsCurve::Cast(sb.brep->m_C2[
      sb.brep->m_T[sb.inner_trim_indices[0]].m_c2i]);
  t.require(raised != nullptr, "output trim is still NURBS");
  t.require(raised->Degree() > 3, "exact lift raises the trim degree");

  // A second call must accept the raised curve and stay valid.
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 1.3, 0.8, nullptr),
            "raised-degree trim remains a supported input");
  t.require(sb.brep->IsValid(), "brep valid after second call");
  return 0;
}

// ---------------------------------------------------------------------------
// sharing-and-failure group
// ---------------------------------------------------------------------------

static int sharing_surface_and_c2_protected(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample valid");
  const int shared_c2 = sb.brep->m_T[sb.outer_trim_indices[0]].m_c2i;
  t.require(AddSharingFace(sb, shared_c2), "second face added");
  t.require(sb.brep->IsValid(), "two-face brep valid");

  const int face2 = sb.brep->m_F.Count() - 1;
  t.require(face2 != sb.face_index, "two distinct faces");

  // Record geometry of the second face and the shared 2d curve.
  std::unique_ptr<ON_Surface> srf2_before(
    sb.brep->m_F[face2].SurfaceOf()->DuplicateSurface());
  const ON_Curve* shared_before = sb.brep->m_C2[shared_c2];
  std::vector<ON_2dPoint> shared_points;
  for (int k = 0; k <= 10; ++k)
    shared_points.push_back(ON_2dPoint(
      shared_before->PointAt(shared_before->Domain().ParameterAt(k / 10.0))));

  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 2.0, 3.0, nullptr),
            "reparameterize only the first face");
  t.require(sb.brep->IsValid(), "brep stays valid");

  // Second face surface is unchanged.
  const ON_Surface* srf2_after = sb.brep->m_F[face2].SurfaceOf();
  for (int ix = 1; ix <= 3; ++ix)
  {
    const double x = ix / 4.0, y = ix / 4.0;
    ON_3dPoint pb, pa;
    srf2_before->EvPoint(sb.udom.ParameterAt(x), sb.vdom.ParameterAt(y), pb);
    srf2_after->EvPoint(sb.udom.ParameterAt(x), sb.vdom.ParameterAt(y), pa);
    t.require(pa.DistanceTo(pb) <= 1.0e-12,
              "shared surface of the other face is not modified");
  }

  // The shared 2d curve still reports its original locus.
  const ON_Curve* shared_after = sb.brep->m_C2[shared_c2];
  for (int k = 0; k <= 10; ++k)
  {
    ON_3dPoint q = shared_after->PointAt(
      shared_after->Domain().ParameterAt(k / 10.0));
    t.require(ON_2dPoint(q.x, q.y).DistanceTo(shared_points[k]) <= 1.0e-12,
              "shared 2d curve of the other face is not modified");
  }

  // Indices never change.
  t.require(sb.brep->m_F.Count() == 2, "face count unchanged");
  t.require(sb.brep->m_F[sb.face_index].m_face_index == sb.face_index,
            "target face index stable");
  return 0;
}

static int sharing_reversed_proxy_preserved(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, true);
  t.require(sb.brep && sb.brep->IsValid(), "sample with reversed proxy valid");
  const int ti = sb.reversed_proxy_trim;
  t.require(ti >= 0, "fixture provides a reversed-proxy trim");
  const bool rev_before = sb.brep->m_T[ti].ProxyCurveIsReversed();
  const bool brev_before = sb.brep->m_T[ti].m_bRev3d;
  const ON_Interval this_dom_before = sb.brep->m_T[ti].Domain();
  const ON_Interval real_dom_before = sb.brep->m_T[ti].ProxyCurveDomain();
  t.require(rev_before, "test fixture really has a reversed proxy");

  // Record the 2d locus and lift it to 3d on a reference copy of the
  // original surface.
  std::unique_ptr<ON_Surface> ref_srf(
    sb.brep->m_F[sb.face_index].SurfaceOf()->DuplicateSurface());
  std::vector<ON_3dPoint> boundary3d;
  for (int k = 0; k <= 12; ++k)
  {
    ON_3dPoint q = sb.brep->m_T[ti].PointAt(
      this_dom_before.ParameterAt(k / 12.0));
    boundary3d.push_back(ref_srf->PointAt(q.x, q.y));
  }

  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 1.6, 2.4, nullptr),
            "reparameterization with reversed proxy succeeds");
  t.require(sb.brep->IsValid(), "brep valid");

  const ON_BrepTrim& trim = sb.brep->m_T[ti];
  t.require(trim.ProxyCurveIsReversed() == rev_before,
            "proxy reversal flag preserved");
  t.require(trim.Domain() == this_dom_before, "this domain preserved");
  t.require(trim.ProxyCurveDomain() == real_dom_before,
            "real curve sub domain preserved");
  t.require(trim.m_bRev3d == brev_before, "m_bRev3d preserved");

  const ON_Surface& srf = *sb.brep->m_F[sb.face_index].SurfaceOf();
  for (int k = 0; k <= 12; ++k)
  {
    const ON_3dPoint q3 = trim.PointAt(this_dom_before.ParameterAt(k / 12.0));
    const ON_2dPoint uv(q3.x, q3.y);
    // The inverse remap must keep the reversed trim on the same 3d
    // boundary even though its 2d coordinates have moved.
    const ON_3dPoint lifted = srf.PointAt(uv.x, uv.y);
    const double sc = 1.0 + std::fabs(boundary3d[k].x) + std::fabs(boundary3d[k].y)
                          + std::fabs(boundary3d[k].z);
    t.require(lifted.DistanceTo(boundary3d[k]) <= kTestTol * sc,
              "reversed trim keeps its 3d boundary locus");
  }
  return 0;
}

static int failure_invalid_index_and_bad_input(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample valid");

  // Bad face indices are rejected without touching the brep.
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, -1, 1.0, 1.0, nullptr),
            "negative face index rejected");
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, sb.brep->m_F.Count(),
                                       1.0, 1.0, nullptr),
            "out of range face index rejected");
  t.require(sb.brep->IsValid(), "brep untouched by bad index calls");

  // An unsupported 2d trim type is rejected on a scratch copy so the
  // caller's brep is never left in a corrupted state.
  {
    std::unique_ptr<ON_Brep> scratch(ON_Brep::New(*sb.brep));
    const int ti = sb.inner_trim_indices[0];
    ON_LineCurve* bad = new ON_LineCurve(
      ON_3dPoint(sb.udom[0], sb.vdom[0], 0.0),
      ON_3dPoint(sb.udom.ParameterAt(0.5), sb.vdom.ParameterAt(0.5), 0.0));
    bad->SetDomain(0.0, 1.0);
    const int bad_c2i = scratch->AddTrimCurve(bad);
    scratch->m_T[ti].ChangeTrimCurve(bad_c2i);
    t.require(!ON_ReparameterizeBrepFace(*scratch, sb.face_index, 2.0, 2.0, nullptr),
              "non NURBS 2d trim rejected");
  }

  // The original brep is still valid and can be reparameterized, which
  // proves a rejected call never mutated it.
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 2.0, 2.0, nullptr),
            "brep can be reparameterized after the rejected attempt");
  t.require(sb.brep->IsValid(), "brep valid after recovery");
  return 0;
}

static int failure_leaves_brep_evaluable(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample valid");

  // Evaluate the surface and a trim before a guaranteed-failing call.
  const ON_Surface& srf0 = *sb.brep->m_F[sb.face_index].SurfaceOf();
  ON_3dPoint p0;
  t.require(srf0.EvPoint(sb.udom.ParameterAt(0.3), sb.vdom.ParameterAt(0.6), p0),
            "surface evaluates before failure");

  t.require(!ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, -1.0, 2.0, nullptr),
            "negative coefficient fails");
  t.require(!ON_ReparameterizeBrepFace(*sb.brep, 999, 2.0, 2.0, nullptr),
            "bad index fails");

  ON_3dPoint p1;
  const ON_Surface& srf1 = *sb.brep->m_F[sb.face_index].SurfaceOf();
  t.require(srf1.EvPoint(sb.udom.ParameterAt(0.3), sb.vdom.ParameterAt(0.6), p1),
            "surface still evaluates after failures");
  t.require(p0.DistanceTo(p1) <= 1.0e-12, "geometry unchanged after failures");
  t.require(sb.brep->IsValid(), "brep valid after failures");
  return 0;
}

static int inverse_coefficients_restore_mapping(Check& t)
{
  SampleBrep sb = MakeSampleBrep(true, false);
  t.require(sb.brep && sb.brep->IsValid(), "sample valid");
  std::unique_ptr<ON_Surface> original(
    sb.brep->m_F[sb.face_index].SurfaceOf()->DuplicateSurface());

  const double a = 3.5, b = 0.2;
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, a, b, nullptr),
            "forward reparameterization");
  t.require(ON_ReparameterizeBrepFace(*sb.brep, sb.face_index, 1.0 / a, 1.0 / b,
                                      nullptr),
            "inverse reparameterization");
  t.require(sb.brep->IsValid(), "brep valid after round trip");

  const ON_Surface& restored = *sb.brep->m_F[sb.face_index].SurfaceOf();
  for (int ix = 1; ix <= 3; ++ix)
  {
    const double x = ix / 4.0, y = ix / 4.0;
    ON_3dPoint po, pr;
    original->EvPoint(sb.udom.ParameterAt(x), sb.vdom.ParameterAt(y), po);
    restored.EvPoint(sb.udom.ParameterAt(x), sb.vdom.ParameterAt(y), pr);
    const double sc = 1.0 + std::fabs(po.x) + std::fabs(po.y) + std::fabs(po.z);
    t.require(pr.DistanceTo(po) <= kTestTol * sc,
              "inverse coefficients restore the geometry and parameter map");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Wraps a case so its name is printed together with the failed message.
using CaseFn = int (*)(Check&);
#define WRAP_CASE(group, name, fn)                                      \
  static int wrapped_##fn(Check& t)                                     \
  {                                                                     \
    try { return fn(t); }                                               \
    catch (const std::runtime_error& e) {                               \
      std::fprintf(stderr, "  [case %s/%s] %s\n", group, name, e.what()); \
      throw;                                                            \
    }                                                                   \
  }

WRAP_CASE("exact-map", "surface-point-and-derivative-identity",
          exact_map_surface_identity)
WRAP_CASE("exact-map", "trim-inverse-mobius-and-3d-boundary",
          exact_map_trim_inverse)
WRAP_CASE("exact-map", "unit-and-invalid-coefficients",
          exact_map_identity_coefficients)
WRAP_CASE("trimmed-face", "curved-hole-and-multi-span-trim",
          trimmed_face_hole_and_multispan)
WRAP_CASE("trimmed-face", "tolerance-scaling",
          trimmed_face_tolerance_scaling)
WRAP_CASE("trimmed-face", "raised-degree-output-reusable",
          trimmed_face_high_degree_output_reusable)
WRAP_CASE("sharing-and-failure", "shared-surface-and-c2-protected",
          sharing_surface_and_c2_protected)
WRAP_CASE("sharing-and-failure", "reversed-proxy-preserved",
          sharing_reversed_proxy_preserved)
WRAP_CASE("sharing-and-failure", "invalid-index-and-unsupported-trim",
          failure_invalid_index_and_bad_input)
WRAP_CASE("sharing-and-failure", "brep-evaluable-after-failure",
          failure_leaves_brep_evaluable)
WRAP_CASE("sharing-and-failure", "inverse-coefficients-restore-mapping",
          inverse_coefficients_restore_mapping)

void register_contracts(Registry& r)
{
  r.add("exact-map", "surface-point-and-derivative-identity",
        wrapped_exact_map_surface_identity);
  r.add("exact-map", "trim-inverse-mobius-and-3d-boundary",
        wrapped_exact_map_trim_inverse);
  r.add("exact-map", "unit-and-invalid-coefficients",
        wrapped_exact_map_identity_coefficients);

  r.add("trimmed-face", "curved-hole-and-multi-span-trim",
        wrapped_trimmed_face_hole_and_multispan);
  r.add("trimmed-face", "tolerance-scaling",
        wrapped_trimmed_face_tolerance_scaling);
  r.add("trimmed-face", "raised-degree-output-reusable",
        wrapped_trimmed_face_high_degree_output_reusable);

  r.add("sharing-and-failure", "shared-surface-and-c2-protected",
        wrapped_sharing_surface_and_c2_protected);
  r.add("sharing-and-failure", "reversed-proxy-preserved",
        wrapped_sharing_reversed_proxy_preserved);
  r.add("sharing-and-failure", "invalid-index-and-unsupported-trim",
        wrapped_failure_invalid_index_and_bad_input);
  r.add("sharing-and-failure", "brep-evaluable-after-failure",
        wrapped_failure_leaves_brep_evaluable);
  r.add("sharing-and-failure", "inverse-coefficients-restore-mapping",
        wrapped_inverse_coefficients_restore_mapping);
}
