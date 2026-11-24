#include "opennurbs_brep_reseam.h"

#include "opennurbs_linecurve.h"
#include "opennurbs_nurbssurface.h"
#include "opennurbs_textlog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {

constexpr double kCoordEps = 1.0e-9;
constexpr double kTraceEps = 1.0e-7;

bool Fail(const char* message, ON_TextLog* log)
{
  if (log && message)
    log->Print("ON_ReseamBrepFace: %s\n", message);
  return false;
}

bool FiniteDouble(double x)
{
  return x == x && x > -std::numeric_limits<double>::infinity()
         && x < std::numeric_limits<double>::infinity();
}

double ScaleEps(double x, double eps = kCoordEps)
{
  return eps * std::max(1.0, std::fabs(x));
}

bool Near(double a, double b, double eps = kCoordEps)
{
  return std::fabs(a - b) <= eps;
}

struct LineInfo
{
  bool valid = false;
  int varying_dir = -1;
  double fixed_value = 0.0;
  double oriented_start = 0.0;
  double oriented_end = 0.0;
  ON_2dPoint p0 = ON_2dPoint(0.0, 0.0);
  ON_2dPoint p1 = ON_2dPoint(0.0, 0.0);
};

bool GetLineInfo(const ON_Curve& curve, LineInfo& info)
{
  const ON_Interval d = curve.Domain();
  if (!d.IsValid() || !d.IsIncreasing()) {
    // Reversed proxies can temporarily have a decreasing domain.  Ask for the
    // two ends through normalized evaluation instead of assuming order.
    if (!d.IsValid() || d[0] == d[1])
      return false;
  }

  info.p0 = curve.PointAt(d[0]);
  info.p1 = curve.PointAt(d[1]);
  if (!info.p0.IsValid() || !info.p1.IsValid())
    return false;

  const double dx = info.p1.x - info.p0.x;
  const double dy = info.p1.y - info.p0.y;
  const double lx = std::fabs(dx);
  const double ly = std::fabs(dy);
  const double scale = std::max({1.0, lx, ly, std::fabs(info.p0.x), std::fabs(info.p0.y),
                                  std::fabs(info.p1.x), std::fabs(info.p1.y)});
  const double axis_eps = 1.0e-8 * scale;

  if (lx <= axis_eps && ly <= axis_eps)
    return false;

  if (lx >= ly) {
    if (std::fabs(info.p0.y - info.p1.y) > axis_eps)
      return false;
    info.varying_dir = 0;
    info.fixed_value = 0.5 * (info.p0.y + info.p1.y);
    info.oriented_start = info.p0.x;
    info.oriented_end = info.p1.x;
  } else {
    if (std::fabs(info.p0.x - info.p1.x) > axis_eps)
      return false;
    info.varying_dir = 1;
    info.fixed_value = 0.5 * (info.p0.x + info.p1.x);
    info.oriented_start = info.p0.y;
    info.oriented_end = info.p1.y;
  }

  // The contract gives exact axis-aligned line loci.  IsLinear also sees
  // through ON_CurveProxy and restricted proxy domains.
  if (!curve.IsLinear(1.0e-8 * scale))
    return false;

  info.valid = true;
  return true;
}

// Restrict a curve proxy to the piece between two parameters in its current
// evaluation orientation.  The resulting domain is the corresponding interval
// on the real curve (reversed when necessary), which is the convention used
// by ON_Brep loop validation against m_C2[].
void RestrictProxyPiece(ON_CurveProxy& proxy, double eval_t0, double eval_t1)
{
  const bool want_reversed = proxy.ProxyCurveIsReversed();
  const ON_Curve* real_curve = proxy.ProxyCurve();
  const double r0 = proxy.RealCurveParameter(eval_t0);
  const double r1 = proxy.RealCurveParameter(eval_t1);
  const double lo = std::min(r0, r1);
  const double hi = std::max(r0, r1);

  proxy.SetProxyCurve(real_curve, ON_Interval(lo, hi));
  if (want_reversed)
    proxy.ON_CurveProxy::Reverse();
}

void RecomputeTrimBox(ON_BrepTrim& trim)
{
  trim.DestroyPspaceInformation();
  ON_BoundingBox box;
  if (trim.GetBoundingBox(box) && box.IsValid()) {
    box.m_min.z = 0.0;
    box.m_max.z = 0.0;
    trim.m_pbox = box;
  }
}

// Replace a target-face trim's 2d line by its periodically unwrapped line.
// The proxy reversal flag is retained by orienting the underlying line
// oppositely when necessary.
void SetMappedTrimLine(ON_Brep& brep, int trim_index, const ON_2dPoint& q0,
                       const ON_2dPoint& q1)
{
  ON_BrepTrim& trim = brep.m_T[trim_index];
  const bool proxy_reversed = trim.ProxyCurveIsReversed();

  ON_2dPoint real0 = q0;
  ON_2dPoint real1 = q1;
  if (proxy_reversed)
    std::swap(real0, real1);

  ON_LineCurve* line = new ON_LineCurve(real0, real1);
  line->SetDomain(0.0, 1.0);
  const int c2i = brep.AddTrimCurve(line);
  trim.m_c2i = c2i;
  trim.SetProxyCurve(brep.m_C2[c2i], ON_Interval(0.0, 1.0));
  if (proxy_reversed)
    trim.ON_CurveProxy::Reverse();
  RecomputeTrimBox(trim);
}

void SetTrimVerticesFromEdge(ON_BrepTrim& trim)
{
  const ON_BrepEdge* edge = trim.Edge();
  if (!edge)
    return;
  if (trim.m_bRev3d) {
    trim.m_vi[0] = edge->m_vi[1];
    trim.m_vi[1] = edge->m_vi[0];
  } else {
    trim.m_vi[0] = edge->m_vi[0];
    trim.m_vi[1] = edge->m_vi[1];
  }
}

struct SplitPlan
{
  int source_edge = -1;
  int work_vertex = -1;
  int work_edge1 = -1;
  double edge_fraction = 0.0;
  double other_value = 0.0;
};

struct Segment
{
  int trim_index = -1;
  int v0 = -1;
  int v1 = -1;
  ON_2dPoint p0;
  ON_2dPoint p1;
};

ON_2dPoint TrimEvalEnd(const ON_BrepTrim& trim, double normalized_end)
{
  const ON_Interval d = trim.Domain();
  return ON_2dPoint(trim.PointAt(d.ParameterAt(normalized_end)));
}

} // namespace

bool ON_ReseamBrepFace(
  const ON_Brep& source,
  int face_index,
  int dir,
  double seam,
  ON_Brep& result,
  ON_ReseamMap& map,
  ON_TextLog* log)
{
  if (&result == reinterpret_cast<ON_Brep*>(const_cast<ON_Brep*>(&source)))
    return Fail("result and source must not alias", log);

  if (!FiniteDouble(seam))
    return Fail("non-finite seam parameter", log);
  if (dir != 0 && dir != 1)
    return Fail("dir must be 0 or 1", log);
  if (face_index < 0 || face_index >= source.m_F.Count())
    return Fail("face index is out of range", log);
  if (!source.IsValid(log))
    return Fail("source BRep is not valid", log);

  const int other = 1 - dir;
  const ON_BrepFace& source_face = source.m_F[face_index];
  if (source_face.m_si < 0 || source_face.m_si >= source.m_S.Count() || !source.m_S[source_face.m_si])
    return Fail("face has no surface", log);

  const ON_NurbsSurface* nurbs = ON_NurbsSurface::Cast(source.m_S[source_face.m_si]);
  if (!nurbs)
    return Fail("target surface is not ON_NurbsSurface", log);
  if (nurbs->Dimension() != 3)
    return Fail("target surface is not three-dimensional", log);
  if (nurbs->Degree(0) < 1 || nurbs->Degree(0) > 3 || nurbs->Degree(1) < 1 || nurbs->Degree(1) > 3)
    return Fail("surface degrees must be 1..3", log);
  if (!nurbs->IsClosed(dir))
    return Fail("dir surface direction is not closed", log);
  if (nurbs->IsClosed(other))
    return Fail("other surface direction must be open", log);
  for (int side = 0; side < 4; ++side) {
    if (nurbs->IsSingular(side))
      return Fail("singular surface sides are not supported", log);
  }

  if (nurbs->m_is_rat) {
    for (int i = 0; i < nurbs->m_cv_count[0]; ++i) {
      for (int j = 0; j < nurbs->m_cv_count[1]; ++j) {
        const double w = nurbs->Weight(i, j);
        if (!FiniteDouble(w) || w <= 0.0)
          return Fail("surface weights must be finite and positive", log);
      }
    }
  }

  const ON_Interval old_domain = nurbs->Domain(dir);
  const ON_Interval other_domain = nurbs->Domain(other);
  if (!old_domain.IsValid() || !old_domain.IsIncreasing() || old_domain.Length() <= 0.0)
    return Fail("invalid periodic domain", log);
  if (!other_domain.IsValid() || !other_domain.IsIncreasing() || other_domain.Length() <= 0.0)
    return Fail("invalid open domain", log);

  const double a = old_domain[0];
  const double b = old_domain[1];
  const double period = b - a;
  if (!(seam > a && seam < b))
    return Fail("seam must be strictly inside the periodic domain", log);

  if (source_face.m_li.Count() <= 0)
    return Fail("target face has no loops", log);

  std::vector<int> face_trim_indices;
  std::set<int> face_loop_indices;
  std::set<int> old_seam_trim_set;
  std::set<int> old_seam_edge_set;
  std::map<int, std::vector<int>> seam_trims_by_edge;

  for (int fli = 0; fli < source_face.m_li.Count(); ++fli) {
    const int li = source_face.m_li[fli];
    if (li < 0 || li >= source.m_L.Count() || source.m_L[li].m_loop_index != li
        || source.m_L[li].m_fi != face_index)
      return Fail("invalid target face loop", log);
    face_loop_indices.insert(li);
    const ON_BrepLoop& loop = source.m_L[li];
    for (int k = 0; k < loop.m_ti.Count(); ++k) {
      const int ti = loop.m_ti[k];
      if (ti < 0 || ti >= source.m_T.Count() || source.m_T[ti].m_trim_index != ti
          || source.m_T[ti].m_li != li)
        return Fail("invalid target face trim", log);
      face_trim_indices.push_back(ti);

      const ON_BrepTrim& trim = source.m_T[ti];
      LineInfo info;
      if (!GetLineInfo(trim, info))
        return Fail("target C2 must be an exact axis-aligned line", log);

      if (trim.m_type == ON_BrepTrim::seam) {
        if (trim.m_ei < 0 || trim.m_ei >= source.m_E.Count())
          return Fail("seam trim has no edge", log);
        if (info.varying_dir != other)
          return Fail("seam trim has wrong axis", log);
        const double d_value = 0.5 * (trim.PointAtStart().x + trim.PointAtEnd().x
                                      + trim.PointAt(trim.Domain()[0]).x
                                      + trim.PointAt(trim.Domain()[1]).x) * 0.25;
        const double coord = (dir == 0) ? info.fixed_value : info.fixed_value;
        (void)d_value;
        if (!Near(coord, a, ScaleEps(a, 1e-8)) && !Near(coord, b, ScaleEps(b, 1e-8)))
          return Fail("old seam is not on a or b side", log);
        old_seam_trim_set.insert(ti);
        old_seam_edge_set.insert(trim.m_ei);
        seam_trims_by_edge[trim.m_ei].push_back(ti);
      }
    }
  }

  if (old_seam_edge_set.empty())
    return Fail("target face has no parameter seam", log);

  for (int ei : old_seam_edge_set) {
    const ON_BrepEdge& edge = source.m_E[ei];
    if (edge.m_edge_index != ei || edge.m_ti.Count() != 2)
      return Fail("old seam edge must be a two-trim manifold seam", log);
    const auto found = seam_trims_by_edge.find(ei);
    if (found == seam_trims_by_edge.end() || found->second.size() != 2)
      return Fail("old seam edge must have a reverse seam trim pair", log);
    const int t0 = found->second[0];
    const int t1 = found->second[1];
    if (source.m_T[t0].m_bRev3d == source.m_T[t1].m_bRev3d)
      return Fail("old seam trims must have opposite 3d orientation", log);
    std::set<int> edge_trim_set;
    for (int k = 0; k < edge.m_ti.Count(); ++k)
      edge_trim_set.insert(edge.m_ti[k]);
    if (edge_trim_set.size() != 2 || !edge_trim_set.count(t0) || !edge_trim_set.count(t1))
      return Fail("old seam edge trim membership is invalid", log);
  }

  // Plan one split for every source edge crossed by the new seam.
  std::map<int, SplitPlan> plans_by_edge;
  std::vector<double> event_other_values;

  for (int ti : face_trim_indices) {
    if (old_seam_trim_set.count(ti))
      continue;

    const ON_BrepTrim& trim = source.m_T[ti];
    LineInfo info;
    if (!GetLineInfo(trim, info))
      return Fail("target C2 must be an exact axis-aligned line", log);

    const double p0d = (dir == 0) ? info.p0.x : info.p0.y;
    const double p1d = (dir == 0) ? info.p1.x : info.p1.y;

    if (info.varying_dir == dir) {
      const double d_low = std::min(p0d, p1d);
      const double d_high = std::max(p0d, p1d);
      if (seam < d_low || seam > d_high)
        continue;

      const double denom = p1d - p0d;
      if (!FiniteDouble(denom) || std::fabs(denom) <= 1.0e-14)
        return Fail("degenerate transverse trim", log);
      double oriented_f = (seam - p0d) / denom;
      // Endpoint events are accepted only when two collinear pieces meet at
      // a periodic join created by an earlier reseam.  The post-plan check
      // enforces that; a lone endpoint is a real corner and is rejected.
      if (Near(seam, p0d, ScaleEps(p0d, 1e-9)))
        oriented_f = 0.0;
      else if (Near(seam, p1d, ScaleEps(p1d, 1e-9)))
        oriented_f = 1.0;
      if (!FiniteDouble(oriented_f) || oriented_f < 0.0 || oriented_f > 1.0)
        return Fail("invalid transverse event parameter", log);

      const int ei = trim.m_ei;
      if (ei < 0 || ei >= source.m_E.Count() || source.m_E[ei].m_edge_index != ei)
        return Fail("crossing trim has no real edge", log);
      if (old_seam_edge_set.count(ei))
        return Fail("internal crossing cannot use old seam edge", log);
      if (source.m_E[ei].m_ti.Count() < 1 || source.m_E[ei].m_ti.Count() > 2)
        return Fail("only compact oriented manifold edges are supported", log);

      const double edge_f = trim.m_bRev3d ? 1.0 - oriented_f : oriented_f;
      const double other_value = info.fixed_value;
      auto prior = plans_by_edge.find(ei);
      if (prior != plans_by_edge.end()) {
        if (!Near(prior->second.edge_fraction, edge_f, 1.0e-8))
          return Fail("inconsistent shared-edge split parameters", log);
        if (!Near(prior->second.other_value, other_value, ScaleEps(other_value, 1e-8)))
          return Fail("inconsistent split event coordinates", log);
      } else {
        SplitPlan plan;
        plan.source_edge = ei;
        plan.edge_fraction = edge_f;
        plan.other_value = other_value;
        plans_by_edge.emplace(ei, plan);
        event_other_values.push_back(other_value);
      }
    } else if (info.varying_dir == other) {
      if (Near(info.fixed_value, seam, ScaleEps(seam, 1e-8)))
        return Fail("new seam coincides with a real contour", log);
    } else {
      return Fail("unsupported target trim direction", log);
    }
  }

  std::sort(event_other_values.begin(), event_other_values.end());
  event_other_values.erase(std::unique(event_other_values.begin(), event_other_values.end(),
                                       [](double x, double y) {
                                         return Near(x, y, 1.0e-9 * std::max(1.0, std::fabs(x)));
                                       }),
                           event_other_values.end());
  if (event_other_values.size() < 2 || (event_other_values.size() % 2) != 0)
    return Fail("seam events do not bound material intervals", log);
  if (!Near(event_other_values.front(), other_domain[0],
            ScaleEps(other_domain[0], 1e-7))
      || !Near(event_other_values.back(), other_domain[1],
               ScaleEps(other_domain[1], 1e-7)))
    return Fail("seam events do not reach both open domain ends", log);

  for (size_t i = 1; i < event_other_values.size(); ++i) {
    const double gap = event_other_values[i] - event_other_values[i - 1];
    if (!(gap > 1.0e-9 * std::max(1.0, other_domain.Length())))
      return Fail("transverse seam events are not separated", log);
  }

  for (double event_value : event_other_values) {
    int endpoint_plans = 0;
    for (const auto& entry : plans_by_edge) {
      const SplitPlan& plan = entry.second;
      if ((plan.edge_fraction <= 1.0e-12 || plan.edge_fraction >= 1.0 - 1.0e-12)
          && Near(plan.other_value, event_value,
                  ScaleEps(event_value, 1e-8))) {
        ++endpoint_plans;
      }
    }
    if (endpoint_plans == 1)
      return Fail("new seam passes through an original corner vertex", log);
  }

  // All validation is complete.  Work on a local copy so a later failure
  // cannot touch the caller's result or map.
  ON_Brep work;
  work = source;

  ON_ReseamMap work_map;
  work_map.edge_images.resize(source.m_E.Count());
  work_map.trim_images.resize(source.m_T.Count());
  for (int i = 0; i < source.m_E.Count(); ++i)
    work_map.edge_images[i] = {i};
  for (int i = 0; i < source.m_T.Count(); ++i)
    work_map.trim_images[i] = {i};

  std::map<int, double> edge_order;
  std::map<int, double> trim_order;
  std::set<int> changed_faces{face_index};

  // Split source edges and propagate every split to every adjacent trim.
  for (auto& plan_pair : plans_by_edge) {
    SplitPlan& plan = plan_pair.second;
    const int eid = plan.source_edge;
    ON_BrepEdge& source_like_edge = work.m_E[eid];
    const ON_Interval edge_domain = source_like_edge.Domain();
    if (!edge_domain.IsValid())
      return Fail("invalid edge domain", log);
    const double edge_t = edge_domain.ParameterAt(plan.edge_fraction);
    if (!FiniteDouble(edge_t))
      return Fail("invalid edge split parameter", log);

    if (plan.edge_fraction <= 1.0e-12 || plan.edge_fraction >= 1.0 - 1.0e-12) {
      // Returning to a previous periodic cut reuses its join vertex instead
      // of splitting an edge at an endpoint.
      plan.work_vertex = (plan.edge_fraction <= 0.5)
        ? source_like_edge.m_vi[0]
        : source_like_edge.m_vi[1];
      if (plan.work_vertex < 0 || plan.work_vertex >= work.m_V.Count())
        return Fail("endpoint event has no vertex", log);
      continue;
    }

    ON_3dPoint split_point = source_like_edge.PointAt(edge_t);
    if (!split_point.IsValid())
      return Fail("could not evaluate edge split point", log);

    plan.work_vertex = work.NewVertex(split_point).m_vertex_index;
    const int new_eid = work.NewEdge(source_like_edge.m_c3i).m_edge_index;
    plan.work_edge1 = new_eid;

    // NewEdge may reallocate m_E; copy the original proxy state by index.
    work.m_E[new_eid].ON_CurveProxy::operator=(work.m_E[eid]);
    work.m_E[new_eid].m_edge_index = new_eid;
    work.m_E[new_eid].m_ti.Empty();

    edge_order[eid] = 0.5 * (std::min(edge_domain[0], edge_t) + std::max(edge_domain[0], edge_t));
    edge_order[new_eid] = 0.5 * (std::min(edge_t, edge_domain[1]) + std::max(edge_t, edge_domain[1]));

    RestrictProxyPiece(work.m_E[eid], edge_domain[0], edge_t);
    RestrictProxyPiece(work.m_E[new_eid], edge_t, edge_domain[1]);

    const int v0 = work.m_E[eid].m_vi[0];
    const int v1 = work.m_E[eid].m_vi[1];
    work.m_E[eid].m_vi[0] = v0;
    work.m_E[eid].m_vi[1] = plan.work_vertex;
    work.m_E[new_eid].m_vi[0] = plan.work_vertex;
    work.m_E[new_eid].m_vi[1] = v1;
    work.m_E[eid].m_ti.Empty();

    work_map.edge_images[eid].push_back(new_eid);

    // Use the source edge's trim list; every index is initially identical in
    // the copied BRep and no trim belongs to two edges.
    std::vector<int> adjacent_trims;
    adjacent_trims.reserve(source.m_E[eid].m_ti.Count());
    for (int k = 0; k < source.m_E[eid].m_ti.Count(); ++k)
      adjacent_trims.push_back(source.m_E[eid].m_ti[k]);
    for (const int ti : adjacent_trims) {
      if (ti < 0 || ti >= work.m_T.Count())
        return Fail("invalid adjacent trim during edge split", log);
      const int old_c2i = work.m_T[ti].m_c2i;
      const bool trim_reversed_3d = work.m_T[ti].m_bRev3d;
      const double trim_fraction = trim_reversed_3d ? 1.0 - plan.edge_fraction : plan.edge_fraction;
      const ON_Interval trim_domain = work.m_T[ti].Domain();
      const double trim_t = trim_domain.ParameterAt(trim_fraction);
      if (!FiniteDouble(trim_t))
        return Fail("invalid trim split parameter", log);

      const int li = work.m_T[ti].m_li;
      const int new_ti = work.NewTrim(old_c2i).m_trim_index;
      // NewTrim can reallocate m_T; copy by index and only then take refs.
      work.m_T[new_ti] = work.m_T[ti];
      ON_BrepTrim& old_trim = work.m_T[ti];
      ON_BrepTrim& new_trim = work.m_T[new_ti];
      new_trim.m_trim_index = new_ti;
      new_trim.m_ei = new_eid;
      new_trim.m_li = li;

      trim_order[ti] = 0.5 * (std::min(trim_domain[0], trim_t) + std::max(trim_domain[0], trim_t));
      trim_order[new_ti] = 0.5 * (std::min(trim_t, trim_domain[1]) + std::max(trim_t, trim_domain[1]));

      RestrictProxyPiece(old_trim, trim_domain[0], trim_t);
      RestrictProxyPiece(new_trim, trim_t, trim_domain[1]);

      old_trim.m_bRev3d = trim_reversed_3d;
      new_trim.m_bRev3d = trim_reversed_3d;
      if (!trim_reversed_3d) {
        old_trim.m_ei = eid;
        new_trim.m_ei = new_eid;
      } else {
        // The first piece of a reversed trim traverses the high-parameter
        // edge piece in reverse; the second piece traverses the low piece.
        old_trim.m_ei = new_eid;
        new_trim.m_ei = eid;
      }
      SetTrimVerticesFromEdge(old_trim);
      SetTrimVerticesFromEdge(new_trim);
      RecomputeTrimBox(old_trim);
      RecomputeTrimBox(new_trim);

      if (li >= 0 && li < work.m_L.Count()) {
        ON_BrepLoop& loop = work.m_L[li];
        if (loop.m_fi >= 0 && loop.m_fi < work.m_F.Count())
          changed_faces.insert(loop.m_fi);
        int position = -1;
        for (int k = 0; k < loop.m_ti.Count(); ++k) {
          if (loop.m_ti[k] == ti) {
            position = k;
            break;
          }
        }
        if (position >= 0)
          loop.m_ti.Insert(position + 1, new_ti);
        else
          loop.m_ti.Append(new_ti);
      }

      work_map.trim_images[ti].push_back(new_ti);
    }
  }

  // Duplicate the surface only for the target face.  Other faces may keep
  // using the unchanged original surface index.
  ON_NurbsSurface moved_surface(*nurbs);
  if (!moved_surface.ChangeSurfaceSeam(dir, seam))
    return Fail("ON_NurbsSurface::ChangeSurfaceSeam failed", log);
  const ON_Interval new_domain = moved_surface.Domain(dir);
  if (!Near(new_domain[0], seam, ScaleEps(seam, 1e-10))
      || !Near(new_domain[1], seam + period, ScaleEps(seam + period, 1e-10)))
    return Fail("moved surface has the wrong effective domain", log);

  const int new_si = work.AddSurface(new ON_NurbsSurface(moved_surface));
  if (new_si < 0)
    return Fail("could not add moved surface", log);
  work.m_F[face_index].m_si = new_si;
  work.m_F[face_index].SetProxySurface(work.m_S[new_si]);

  auto event_vertex_at = [&](double value) -> int {
    int best = -1;
    double best_dist = std::numeric_limits<double>::infinity();
    for (const auto& entry : plans_by_edge) {
      const SplitPlan& p = entry.second;
      const double dist = std::fabs(p.other_value - value);
      if (dist < best_dist) {
        best_dist = dist;
        best = p.work_vertex;
      }
    }
    return best_dist <= 1.0e-7 * std::max(1.0, other_domain.Length()) ? best : -1;
  };

  // Transform all retained target trims into [seam, seam+P].  Pieces below
  // the new cut get one period; pieces at/above it stay put.
  for (int ti : face_trim_indices) {
    if (old_seam_trim_set.count(ti))
      continue;
    for (int wti : work_map.trim_images[ti]) {
      ON_BrepTrim& trim = work.m_T[wti];
      const ON_Interval d = trim.Domain();
      ON_3dPoint p0_3 = trim.PointAt(d[0]);
      ON_3dPoint p1_3 = trim.PointAt(d[1]);
      ON_2dPoint p0(p0_3.x, p0_3.y);
      ON_2dPoint p1(p1_3.x, p1_3.y);
      const double mid_d = dir == 0 ? 0.5 * (p0.x + p1.x) : 0.5 * (p0.y + p1.y);
      const double shift = (mid_d < seam) ? period : 0.0;

      auto map_coord = [&](ON_2dPoint p) {
        double& xd = (dir == 0) ? p.x : p.y;
        double& xo = (dir == 0) ? p.y : p.x;
        xd += shift;

        const double candidates[3] = {seam, b, seam + period};
        for (double c : candidates) {
          if (Near(xd, c, 1.0e-8 * std::max(1.0, period))) {
            xd = c;
            break;
          }
        }
        int ev = event_vertex_at(xo);
        if (ev >= 0) {
          // Recover canonical event coordinate from plans.
          double best_value = xo;
          double best_dist = std::numeric_limits<double>::infinity();
          for (const auto& entry : plans_by_edge) {
            const SplitPlan& sp = entry.second;
            if (sp.work_vertex != ev)
              continue;
            const double dd = std::fabs(sp.other_value - xo);
            if (dd < best_dist) {
              best_dist = dd;
              best_value = sp.other_value;
            }
          }
          if (best_dist < kTraceEps)
            xo = best_value;
        }
        return p;
      };

      ON_2dPoint q0 = map_coord(p0);
      ON_2dPoint q1 = map_coord(p1);
      SetMappedTrimLine(work, wti, q0, q1);
    }
  }

  // Remove old seam topology.
  for (int ti : old_seam_trim_set) {
    ON_BrepTrim& trim = work.m_T[ti];
    trim.RemoveFromEdge(false, false);
    trim.m_trim_index = -1;
    work_map.trim_images[ti].clear();
  }
  for (int ei : old_seam_edge_set) {
    work.m_E[ei].m_edge_index = -1;
    work_map.edge_images[ei].clear();
  }

  // Create one exact isocurve C3 and seam edge pieces for material intervals.
  ON_Curve* seam_iso = moved_surface.IsoCurve(other, seam);
  if (!seam_iso)
    return Fail("could not create exact seam isocurve", log);
  const int seam_c3i = work.AddEdgeCurve(seam_iso);
  if (seam_c3i < 0)
    return Fail("could not add seam C3", log);

  std::vector<int> new_seam_trims;
  const ON_Surface::ISO min_iso = (dir == 0) ? ON_Surface::W_iso : ON_Surface::S_iso;
  const ON_Surface::ISO max_iso = (dir == 0) ? ON_Surface::E_iso : ON_Surface::N_iso;

  for (size_t i = 0; i + 1 < event_other_values.size(); i += 2) {
    // Even intervals are material: open-domain start -> first crossing,
    // second crossing -> third crossing, etc.
    const double r0 = event_other_values[i];
    const double r1 = event_other_values[i + 1];
    const int v0 = event_vertex_at(r0);
    const int v1 = event_vertex_at(r1);
    if (v0 < 0 || v1 < 0)
      return Fail("missing seam endpoint vertex", log);

    const int sei = work.NewEdge(seam_c3i).m_edge_index;
    ON_BrepEdge& seam_edge = work.m_E[sei];
    seam_edge.m_vi[0] = v0;
    seam_edge.m_vi[1] = v1;
    seam_edge.SetProxyCurve(work.m_C3[seam_c3i], ON_Interval(r0, r1));
    work_map.seam_edges.push_back(sei);

    auto add_seam_trim = [&](bool min_side) {
      const double fixed_d = min_side ? seam : seam + period;
      ON_2dPoint q0, q1;
      // In a CCW outer loop, the west/min-dir side is traversed high->low
      // for dir 0, while the north/max-dir side is traversed high->low for
      // dir 1.  The other new seam side follows r0 -> r1.
      const bool rev3d = min_side ? (dir == 0) : (dir == 1);
      if (!rev3d) {
        q0 = (dir == 0) ? ON_2dPoint(fixed_d, r0) : ON_2dPoint(r0, fixed_d);
        q1 = (dir == 0) ? ON_2dPoint(fixed_d, r1) : ON_2dPoint(r1, fixed_d);
      } else {
        q0 = (dir == 0) ? ON_2dPoint(fixed_d, r1) : ON_2dPoint(r1, fixed_d);
        q1 = (dir == 0) ? ON_2dPoint(fixed_d, r0) : ON_2dPoint(r0, fixed_d);
      }
      const ON_Surface::ISO iso = min_side ? min_iso : max_iso;

      ON_LineCurve* c2 = new ON_LineCurve(q0, q1);
      c2->SetDomain(0.0, 1.0);
      const int c2i = work.AddTrimCurve(c2);
      const int ti = work.NewTrim(c2i).m_trim_index;
      ON_BrepTrim& trim = work.m_T[ti];
      trim.m_ei = sei;
      trim.m_bRev3d = rev3d;
      trim.m_vi[0] = rev3d ? v1 : v0;
      trim.m_vi[1] = rev3d ? v0 : v1;
      trim.m_type = ON_BrepTrim::seam;
      trim.m_iso = iso;
      trim.m_li = -1;
      trim.SetProxyCurve(work.m_C2[c2i], ON_Interval(0.0, 1.0));
      RecomputeTrimBox(trim);
      new_seam_trims.push_back(ti);
      return ti;
    };

    add_seam_trim(true);
    add_seam_trim(false);
  }

  // Build the pool of target trims and trace closed UV cycles by identical
  // topology vertices plus matching UV endpoints.
  std::vector<int> pool;
  for (int ti : face_trim_indices) {
    if (old_seam_trim_set.count(ti))
      continue;
    for (int wti : work_map.trim_images[ti])
      pool.push_back(wti);
  }
  for (int ti : new_seam_trims)
    pool.push_back(ti);

  std::vector<Segment> segments;
  segments.reserve(pool.size());
  for (int ti : pool) {
    const ON_BrepTrim& trim = work.m_T[ti];
    const ON_Interval d = trim.Domain();
    Segment seg;
    seg.trim_index = ti;
    seg.v0 = trim.m_vi[0];
    seg.v1 = trim.m_vi[1];
    seg.p0 = TrimEvalEnd(trim, 0.0);
    seg.p1 = TrimEvalEnd(trim, 1.0);
    if (seg.v0 < 0 || seg.v1 < 0 || !seg.p0.IsValid() || !seg.p1.IsValid())
      return Fail("invalid traced trim endpoint", log);
    segments.push_back(seg);
  }

  std::vector<std::vector<int>> cycles;
  std::set<int> used_trims;
  for (const Segment& start_seg : segments) {
    const int start_ti = start_seg.trim_index;
    if (used_trims.count(start_ti))
      continue;

    std::vector<int> cycle{start_ti};
    used_trims.insert(start_ti);
    int current_ti = start_ti;

    for (size_t step = 0; step <= segments.size(); ++step) {
      const Segment* current = nullptr;
      for (const Segment& s : segments) {
        if (s.trim_index == current_ti) {
          current = &s;
          break;
        }
      }
      if (!current)
        return Fail("trace lost current trim", log);

      int next_ti = -1;
      double best_dist = kTraceEps;
      for (const Segment& candidate : segments) {
        const int cti = candidate.trim_index;
        if (cti == current_ti)
          continue;
        const bool is_close = (cti == start_ti);
        if (!is_close && used_trims.count(cti))
          continue;
        if (candidate.v0 != current->v1)
          continue;
        const double dist = candidate.p0.DistanceTo(current->p1);
        if (dist <= best_dist) {
          best_dist = dist;
          next_ti = cti;
        }
      }

      if (next_ti < 0)
        return Fail("could not connect resewn UV cycle", log);
      if (next_ti == start_ti)
        break;

      cycle.push_back(next_ti);
      used_trims.insert(next_ti);
      current_ti = next_ti;
      if (cycle.size() > segments.size())
        return Fail("resewn cycle did not close", log);
    }

    if (cycle.size() < 3)
      return Fail("resewn loop has too few trims", log);
    cycles.push_back(cycle);
  }

  if (used_trims.size() != segments.size() || cycles.empty())
    return Fail("not all target trims formed loops", log);

  size_t outer_cycle = 0;
  double outer_area = -1.0;
  std::vector<double> cycle_areas(cycles.size(), 0.0);
  for (size_t ci = 0; ci < cycles.size(); ++ci) {
    double twice_area = 0.0;
    for (int ti : cycles[ci]) {
      const ON_BrepTrim& trim = work.m_T[ti];
      const ON_Interval d = trim.Domain();
      ON_2dPoint p0 = TrimEvalEnd(trim, 0.0);
      ON_2dPoint p1 = TrimEvalEnd(trim, 1.0);
      twice_area += p0.x * p1.y - p1.x * p0.y;
      (void)d;
    }
    cycle_areas[ci] = twice_area;
    if (std::fabs(twice_area) > outer_area) {
      outer_area = std::fabs(twice_area);
      outer_cycle = ci;
    }
  }

  // Discard old loop topology, but retain and reorder its trims.
  for (int k = 0; k < source_face.m_li.Count(); ++k) {
    const int li = source_face.m_li[k];
    if (li >= 0 && li < work.m_L.Count())
      work.m_L[li].m_loop_index = -1;
  }
  work.m_F[face_index].m_li.Empty();

  auto make_loop = [&](const std::vector<int>& cycle, ON_BrepLoop::TYPE type) {
    const int li = work.NewLoop(type).m_loop_index;
    ON_BrepLoop& loop = work.m_L[li];
    loop.m_fi = face_index;
    loop.m_ti.Empty();
    ON_BoundingBox box;
    box.Destroy();
    for (int ti : cycle) {
      loop.m_ti.Append(ti);
      work.m_T[ti].m_li = li;
      box.Union(work.m_T[ti].m_pbox);
    }
    loop.m_pbox = box;
    work.m_F[face_index].m_li.Append(li);
    return li;
  };

  make_loop(cycles[outer_cycle], ON_BrepLoop::outer);
  for (size_t ci = 0; ci < cycles.size(); ++ci) {
    if (ci != outer_cycle)
      make_loop(cycles[ci], ON_BrepLoop::inner);
  }

  // Rebuild reverse topology lists from the authoritative m_ei/m_vi fields.
  for (int ei = 0; ei < work.m_E.Count(); ++ei) {
    ON_BrepEdge& edge = work.m_E[ei];
    if (edge.m_edge_index >= 0)
      edge.m_ti.Empty();
  }
  for (int ti = 0; ti < work.m_T.Count(); ++ti) {
    ON_BrepTrim& trim = work.m_T[ti];
    if (trim.m_trim_index < 0 || trim.m_ei < 0)
      continue;
    if (trim.m_ei >= work.m_E.Count())
      return Fail("trim has invalid edge after reseam", log);
    work.m_E[trim.m_ei].m_ti.Append(ti);
  }
  for (int vi = 0; vi < work.m_V.Count(); ++vi) {
    ON_BrepVertex& vertex = work.m_V[vi];
    if (vertex.m_vertex_index >= 0)
      vertex.m_ei.Empty();
  }
  for (int ei = 0; ei < work.m_E.Count(); ++ei) {
    const ON_BrepEdge& edge = work.m_E[ei];
    if (edge.m_edge_index < 0)
      continue;
    if (edge.m_vi[0] < 0 || edge.m_vi[0] >= work.m_V.Count()
        || edge.m_vi[1] < 0 || edge.m_vi[1] >= work.m_V.Count())
      return Fail("edge has invalid vertices after reseam", log);
    work.m_V[edge.m_vi[0]].m_ei.Append(ei);
    work.m_V[edge.m_vi[1]].m_ei.Append(ei);
  }

  for (int ti = 0; ti < work.m_T.Count(); ++ti) {
    ON_BrepTrim& trim = work.m_T[ti];
    if (trim.m_trim_index >= 0)
      RecomputeTrimBox(trim);
  }

  for (int fi : changed_faces) {
    if (fi >= 0 && fi < work.m_F.Count()) {
      work.m_F[fi].DestroyMesh(ON::any_mesh);
      work.m_F[fi].DestroyRuntimeCache();
    }
  }
  work.ClearBoundingBox();

  work.SetTrimIsoFlags();
  work.SetTrimTypeFlags(false);

  // OpenNURBS' generic SetEdgeTolerance() intentionally leaves edges with
  // trims at ON_UNSET_VALUE for derived kernels to refine.  Compute an
  // actual bound here so IsValid sees recomputed, non-negative tolerances.
  for (int ei = 0; ei < work.m_E.Count(); ++ei) {
    ON_BrepEdge& edge = work.m_E[ei];
    if (edge.m_edge_index < 0)
      continue;
    double error = 0.0;
    const ON_Interval ed = edge.Domain();
    for (int sample = 0; sample <= 17; ++sample) {
      const double ne = sample / 17.0;
      const ON_3dPoint ep = edge.PointAt(ed.ParameterAt(ne));
      for (int k = 0; k < edge.m_ti.Count(); ++k) {
        const int ti = edge.m_ti[k];
        if (ti < 0 || ti >= work.m_T.Count())
          continue;
        const ON_BrepTrim& trim = work.m_T[ti];
        const ON_Surface* surface = trim.SurfaceOf();
        if (!surface)
          continue;
        const double nt = trim.m_bRev3d ? 1.0 - ne : ne;
        const ON_Interval td = trim.Domain();
        const ON_3dPoint uv = trim.PointAt(td.ParameterAt(nt));
        const ON_3dPoint sp = surface->PointAt(uv.x, uv.y);
        if (ep.IsValid() && sp.IsValid())
          error = std::max(error, ep.DistanceTo(sp));
      }
    }
    for (int evi = 0; evi < 2; ++evi) {
      const int vi = edge.m_vi[evi];
      if (vi >= 0 && vi < work.m_V.Count())
        error = std::max(error, edge.PointAt(ed[evi]).DistanceTo(work.m_V[vi].point));
    }
    edge.m_tolerance = (error <= ON_ZERO_TOLERANCE) ? 0.0 : 1.001 * error;
  }

  work.SetVertexTolerances(false);
  work.SetTrimTolerances(false);
  for (int vi = 0; vi < work.m_V.Count(); ++vi) {
    if (work.m_V[vi].m_vertex_index >= 0 && work.m_V[vi].m_tolerance < 0.0)
      work.m_V[vi].m_tolerance = 0.0;
  }
  for (int ti = 0; ti < work.m_T.Count(); ++ti) {
    ON_BrepTrim& trim = work.m_T[ti];
    if (trim.m_trim_index >= 0) {
      for (int j = 0; j < 2; ++j) {
        if (!FiniteDouble(trim.m_tolerance[j]) || trim.m_tolerance[j] < 0.0)
          trim.m_tolerance[j] = 0.0;
      }
    }
  }

  // Record compact-index remaps before calling Compact.  Cull operations keep
  // surviving array entries in their original order.
  std::vector<int> edge_remap(work.m_E.Count(), -1);
  std::vector<int> trim_remap(work.m_T.Count(), -1);
  int live_edge_count = 0;
  for (int i = 0; i < work.m_E.Count(); ++i) {
    if (work.m_E[i].m_edge_index >= 0)
      edge_remap[i] = live_edge_count++;
  }
  int live_trim_count = 0;
  for (int i = 0; i < work.m_T.Count(); ++i) {
    if (work.m_T[i].m_trim_index >= 0)
      trim_remap[i] = live_trim_count++;
  }

  work.Compact();
  if (!work.IsValid(log))
    return Fail("resewn BRep failed IsValid", log);

  ON_ReseamMap final_map;
  final_map.edge_images.resize(source.m_E.Count());
  final_map.trim_images.resize(source.m_T.Count());

  for (int src_e = 0; src_e < source.m_E.Count(); ++src_e) {
    if (old_seam_edge_set.count(src_e))
      continue;
    std::vector<int> ids = work_map.edge_images[src_e];
    std::sort(ids.begin(), ids.end(), [&](int x, int y) {
      const auto ix = edge_order.find(x);
      const auto iy = edge_order.find(y);
      const double kx = ix == edge_order.end() ? 0.0 : ix->second;
      const double ky = iy == edge_order.end() ? 0.0 : iy->second;
      if (kx != ky)
        return kx < ky;
      return x < y;
    });
    std::set<int> unique;
    for (int wid : ids) {
      if (wid < 0 || wid >= static_cast<int>(edge_remap.size()) || edge_remap[wid] < 0)
        return Fail("edge image was culled", log);
      const int rid = edge_remap[wid];
      if (!unique.insert(rid).second)
        return Fail("duplicate edge image", log);
      final_map.edge_images[src_e].push_back(rid);
    }
    if (final_map.edge_images[src_e].empty())
      return Fail("non-seam source edge has no image", log);
  }

  for (int src_t = 0; src_t < source.m_T.Count(); ++src_t) {
    if (old_seam_trim_set.count(src_t))
      continue;
    std::vector<int> ids = work_map.trim_images[src_t];
    std::sort(ids.begin(), ids.end(), [&](int x, int y) {
      const auto ix = trim_order.find(x);
      const auto iy = trim_order.find(y);
      const double kx = ix == trim_order.end() ? 0.0 : ix->second;
      const double ky = iy == trim_order.end() ? 0.0 : iy->second;
      if (kx != ky)
        return kx < ky;
      return x < y;
    });
    std::set<int> unique;
    for (int wid : ids) {
      if (wid < 0 || wid >= static_cast<int>(trim_remap.size()) || trim_remap[wid] < 0)
        return Fail("trim image was culled", log);
      const int rid = trim_remap[wid];
      if (!unique.insert(rid).second)
        return Fail("duplicate trim image", log);
      final_map.trim_images[src_t].push_back(rid);
    }
    if (final_map.trim_images[src_t].empty())
      return Fail("non-seam source trim has no image", log);
  }

  for (int wid : work_map.seam_edges) {
    if (wid < 0 || wid >= static_cast<int>(edge_remap.size()) || edge_remap[wid] < 0)
      return Fail("new seam edge was culled", log);
    final_map.seam_edges.push_back(edge_remap[wid]);
  }
  if (final_map.seam_edges.empty())
    return Fail("no new seam edges", log);

  result = work;
  map = std::move(final_map);
  return true;
}
