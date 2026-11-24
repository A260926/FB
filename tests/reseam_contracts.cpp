#include "task_support/check.h"

#include "opennurbs_public.h"
#include "opennurbs_brep_reseam.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

struct OnGuard {
  OnGuard() { ON::Begin(); }
};
static OnGuard on_guard;

struct PHole {
  double center = 0.0;
  double half = 0.0;
  double o0 = 0.0;
  double o1 = 0.0;
};

struct FixtureOptions {
  int dir = 0;
  double seam_fraction = 0.37;
  bool reverse_proxy = false;
  bool adjacency = false;
  bool extra_surface_user = false;
  std::vector<PHole> holes;
};

struct Fixture {
  ON_Brep brep;
  int face = 0;
  int dir = 0;
  double seam = 0.0;
  double a = 0.0, b = 0.0, o0 = 0.0, o1 = 0.0, period = 0.0;
  std::vector<PHole> holes;
  int shared_edge = -1;
};

double DistEps(double a, double b)
{
  return 1.0e-8 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

void AddRectHole(ON_Brep& brep, int fi, int dir, double d0, double o0, double d1, double o1)
{
  ON_BrepFace& face = brep.m_F[fi];
  ON_Surface* surface = brep.m_S[face.m_si];
  ON_2dPoint uv[4];
  if (dir == 0) {
    // Clockwise in raw (u=d, v=o) coordinates.
    uv[0] = ON_2dPoint(d0, o0);
    uv[1] = ON_2dPoint(d0, o1);
    uv[2] = ON_2dPoint(d1, o1);
    uv[3] = ON_2dPoint(d1, o0);
  } else {
    // Clockwise in raw (u=o, v=d) coordinates.
    uv[0] = ON_2dPoint(o0, d0);
    uv[1] = ON_2dPoint(o0, d1);
    uv[2] = ON_2dPoint(o1, d1);
    uv[3] = ON_2dPoint(o1, d0);
  }

  int vid[4];
  for (int i = 0; i < 4; ++i) {
    vid[i] = brep.NewVertex(surface->PointAt(uv[i].x, uv[i].y)).m_vertex_index;
    brep.m_V[vid[i]].m_tolerance = 0.0;
  }

  ON_BrepLoop& loop = brep.NewLoop(ON_BrepLoop::inner, face);
  for (int i = 0; i < 4; ++i) {
    const int j = (i + 1) % 4;
    const bool varies_d = dir == 0
      ? std::fabs(uv[j].x - uv[i].x) > std::fabs(uv[j].y - uv[i].y)
      : std::fabs(uv[j].y - uv[i].y) > std::fabs(uv[j].x - uv[i].x);
    const int iso_dir = varies_d ? dir : 1 - dir;
    const double coord0 = varies_d
      ? (dir == 0 ? uv[i].x : uv[i].y)
      : (dir == 0 ? uv[i].y : uv[i].x);
    const double coord1 = varies_d
      ? (dir == 0 ? uv[j].x : uv[j].y)
      : (dir == 0 ? uv[j].y : uv[j].x);
    const double fixed = varies_d
      ? (dir == 0 ? uv[i].y : uv[i].x)
      : (dir == 0 ? uv[i].x : uv[i].y);
    const ON_Interval seg(std::min(coord0, coord1), std::max(coord0, coord1));
    ON_Curve* iso = surface->IsoCurve(iso_dir, fixed);
    const int c3i = brep.AddEdgeCurve(iso);
    const int ei = brep.NewEdge(c3i).m_edge_index;
    ON_BrepEdge& edge = brep.m_E[ei];
    edge.m_vi[0] = vid[i];
    edge.m_vi[1] = vid[j];
    brep.m_V[vid[i]].m_ei.Append(ei);
    brep.m_V[vid[j]].m_ei.Append(ei);
    edge.SetProxyCurve(brep.m_C3[c3i], seg);
    if (coord0 > coord1)
      edge.ON_CurveProxy::Reverse();
    edge.m_tolerance = 0.0;

    ON_LineCurve* c2 = new ON_LineCurve(uv[i], uv[j]);
    const int c2i = brep.AddTrimCurve(c2);
    ON_BrepTrim& trim = brep.NewTrim(edge, false, loop, c2i);
    trim.m_type = ON_BrepTrim::boundary;
    trim.m_tolerance[0] = trim.m_tolerance[1] = 0.0;
  }
}

int AddAdjacency(ON_Brep& brep)
{
  const int target_eid = 0;
  ON_Plane plane(ON_3dPoint(0.0, 0.0, -1.0), ON_3dVector::ZAxis);
  ON_Cylinder lower(ON_Circle(plane, 1.0), 1.0);
  ON_NurbsSurface ns2;
  if (!lower.GetNurbForm(ns2))
    return -1;
  const int nf = brep.NewFace(ns2)->m_face_index;

  const ON_Interval ed0 = brep.m_E[target_eid].Domain();
  const ON_3dPoint target_mid = brep.m_E[target_eid].PointAt(ed0.ParameterAt(0.5));
  int neighbor_trim = -1;
  int neighbor_edge = -1;
  for (int li = 0; li < brep.m_F[nf].m_li.Count(); ++li) {
    const int loop_index = brep.m_F[nf].m_li[li];
    ON_BrepLoop& loop = brep.m_L[loop_index];
    for (int k = 0; k < loop.m_ti.Count(); ++k) {
      const int ti = loop.m_ti[k];
      const int ei = brep.m_T[ti].m_ei;
      const ON_Interval ed = brep.m_E[ei].Domain();
      const ON_3dPoint mp = brep.m_E[ei].PointAt(ed.ParameterAt(0.5));
      if (mp.DistanceTo(target_mid) < 1.0e-6) {
        neighbor_trim = ti;
        neighbor_edge = ei;
      }
    }
  }
  if (neighbor_trim < 0)
    return -2;

  ON_BrepTrim& nt = brep.m_T[neighbor_trim];
  const int old_vertex = nt.m_vi[0];
  const int shared_vertex = brep.m_E[target_eid].m_vi[0];
  // NewFace's north trim runs opposite target E0.
  const bool rev = true;

  brep.m_E[neighbor_edge].m_edge_index = -1;
  for (int ei = 0; ei < brep.m_E.Count(); ++ei) {
    ON_BrepEdge& edge = brep.m_E[ei];
    if (edge.m_edge_index < 0)
      continue;
    for (int end = 0; end < 2; ++end) {
      if (edge.m_vi[end] == old_vertex)
        edge.m_vi[end] = shared_vertex;
    }
  }
  for (int ti = 0; ti < brep.m_T.Count(); ++ti) {
    for (int end = 0; end < 2; ++end) {
      if (brep.m_T[ti].m_vi[end] == old_vertex)
        brep.m_T[ti].m_vi[end] = shared_vertex;
    }
  }
  brep.m_V[old_vertex].m_ei.Empty();
  brep.m_V[old_vertex].m_tolerance = 0.0;

  nt.m_ei = target_eid;
  nt.m_bRev3d = rev;
  nt.m_vi[0] = rev ? brep.m_E[target_eid].m_vi[1] : brep.m_E[target_eid].m_vi[0];
  nt.m_vi[1] = rev ? brep.m_E[target_eid].m_vi[0] : brep.m_E[target_eid].m_vi[1];
  ON_3dPoint a = nt.PointAtStart();
  ON_3dPoint b = nt.PointAtEnd();
  const int c2i = brep.AddTrimCurve(
    new ON_LineCurve(ON_2dPoint(a.x, a.y), ON_2dPoint(b.x, b.y)));
  nt.ChangeTrimCurve(c2i);
  nt.m_tolerance[0] = nt.m_tolerance[1] = 0.0;
  brep.m_E[target_eid].m_ti.Append(neighbor_trim);

  for (int vi = 0; vi < brep.m_V.Count(); ++vi)
    brep.m_V[vi].m_ei.Empty();
  for (int ei = 0; ei < brep.m_E.Count(); ++ei) {
    const ON_BrepEdge& edge = brep.m_E[ei];
    if (edge.m_edge_index < 0)
      continue;
    brep.m_V[edge.m_vi[0]].m_ei.Append(ei);
    brep.m_V[edge.m_vi[1]].m_ei.Append(ei);
  }

  brep.SetTrimTypeFlags(false);
  brep.SetTrimIsoFlags();
  brep.Compact();
  return 0;
}

bool BuildFixture(Fixture& fx, const FixtureOptions& opt)
{
  fx.dir = opt.dir;
  fx.holes = opt.holes;

  ON_Cylinder cylinder(ON_Circle(ON_Plane::World_xy, 1.0), 2.0);
  ON_NurbsSurface surface;
  if (!cylinder.GetNurbForm(surface))
    return false;
  surface.SetDomain(0, 10.0, 15.0);
  surface.SetDomain(1, -2.0, 4.0);
  if (opt.dir == 1)
    surface.Transpose();

  const ON_Interval dd = surface.Domain(opt.dir);
  const ON_Interval oo = surface.Domain(1 - opt.dir);
  fx.a = dd[0];
  fx.b = dd[1];
  fx.o0 = oo[0];
  fx.o1 = oo[1];
  fx.period = dd.Length();
  fx.seam = dd.ParameterAt(opt.seam_fraction);

  if (!fx.brep.NewFace(surface))
    return false;

  for (const PHole& h : opt.holes) {
    AddRectHole(fx.brep, 0, opt.dir, h.center - h.half, h.o0,
                h.center + h.half, h.o1);
  }

  if (opt.reverse_proxy) {
    // The rectangular fixture already uses restricted, reversed edge proxies
    // on the sides whose topological direction decreases the iso parameter.
    bool has_reversed_edge_proxy = false;
    for (int ei = 0; ei < fx.brep.m_E.Count(); ++ei) {
      if (fx.brep.m_E[ei].ProxyCurveIsReversed())
        has_reversed_edge_proxy = true;
    }
    if (!has_reversed_edge_proxy)
      return false;
  }

  if (opt.adjacency) {
    if (AddAdjacency(fx.brep) != 0)
      return false;
    fx.shared_edge = 0;
  } else {
    fx.brep.SetTrimTypeFlags(false);
    fx.brep.SetTrimIsoFlags();
    fx.brep.Compact();
  }

  if (opt.extra_surface_user) {
    const int si = fx.brep.m_F[0].m_si;
    ON_BrepFace& extra = fx.brep.NewFace(si);
    if (!fx.brep.NewOuterLoop(extra.m_face_index))
      return false;
    ON_Mesh* mesh = new ON_Mesh();
    mesh->SetVertex(0, ON_3dPoint(0.0, 0.0, 0.0));
    mesh->SetVertex(1, ON_3dPoint(1.0, 0.0, 0.0));
    mesh->SetVertex(2, ON_3dPoint(0.0, 1.0, 0.0));
    ON_MeshFace face;
    face.vi[0] = 0; face.vi[1] = 1; face.vi[2] = 2; face.vi[3] = 2;
    mesh->m_F.Append(face);
    fx.brep.m_F[extra.m_face_index].SetMesh(ON::render_mesh, mesh);
    fx.brep.Compact();
  }

  ON_TextLog build_log(stdout);
  return fx.brep.IsValid(&build_log);
}

bool ExpectedMaterial(const Fixture& fx, double d, double o)
{
  if (o < fx.o0 || o > fx.o1)
    return false;
  double q = std::fmod(d - fx.a, fx.period);
  if (q < 0.0)
    q += fx.period;
  const double wrapped = fx.a + q;
  for (const PHole& h : fx.holes) {
    double x = std::fabs(wrapped - h.center);
    x = std::min(x, fx.period - x);
    if (x < h.half && o > h.o0 && o < h.o1)
      return false;
  }
  return true;
}

struct UvSegment {
  double d0, o0, d1, o1;
};

std::vector<UvSegment> TargetSegments(const ON_Brep& brep, int face, int dir)
{
  std::vector<UvSegment> out;
  const ON_BrepFace& f = brep.m_F[face];
  for (int li = 0; li < f.m_li.Count(); ++li) {
    const ON_BrepLoop& loop = brep.m_L[f.m_li[li]];
    for (int k = 0; k < loop.m_ti.Count(); ++k) {
      const ON_BrepTrim& trim = brep.m_T[loop.m_ti[k]];
      const ON_Interval td = trim.Domain();
      const ON_3dPoint p0 = trim.PointAt(td[0]);
      const ON_3dPoint p1 = trim.PointAt(td[1]);
      if (dir == 0)
        out.push_back({p0.x, p0.y, p1.x, p1.y});
      else
        out.push_back({p0.y, p0.x, p1.y, p1.x});
    }
  }
  return out;
}

bool RayInside(const std::vector<UvSegment>& segs, double d, double o)
{
  bool inside = false;
  for (const UvSegment& s : segs) {
    if (std::fabs(s.d0 - s.d1) > 1.0e-9)
      continue;
    if ((s.o0 > o) != (s.o1 > o)) {
      if (s.d0 > d)
        inside = !inside;
    }
  }
  return inside;
}

int CheckMappingShape(const Fixture& fx, const ON_Brep& result, const ON_ReseamMap& map)
{
  TASK_CHECK(result.IsValid());
  TASK_CHECK(result.m_F.Count() == fx.brep.m_F.Count());
  TASK_CHECK(map.edge_images.size() == static_cast<size_t>(fx.brep.m_E.Count()));
  TASK_CHECK(map.trim_images.size() == static_cast<size_t>(fx.brep.m_T.Count()));

  std::set<int> old_seam_edges, old_seam_trims;
  for (int li = 0; li < fx.brep.m_F[fx.face].m_li.Count(); ++li) {
    const ON_BrepLoop& loop = fx.brep.m_L[fx.brep.m_F[fx.face].m_li[li]];
    for (int k = 0; k < loop.m_ti.Count(); ++k) {
      const int ti = loop.m_ti[k];
      if (fx.brep.m_T[ti].m_type == ON_BrepTrim::seam) {
        old_seam_trims.insert(ti);
        old_seam_edges.insert(fx.brep.m_T[ti].m_ei);
      }
    }
  }

  std::vector<int> edge_used(result.m_E.Count(), 0);
  std::vector<int> trim_used(result.m_T.Count(), 0);
  for (int e = 0; e < fx.brep.m_E.Count(); ++e) {
    if (old_seam_edges.count(e)) {
      TASK_CHECK(map.edge_images[e].empty());
      continue;
    }
    TASK_CHECK(!map.edge_images[e].empty());
    int previous = -1;
    for (int rid : map.edge_images[e]) {
      TASK_CHECK(rid >= 0 && rid < result.m_E.Count());
      TASK_CHECK(rid != previous);
      TASK_CHECK(edge_used[rid] == 0);
      edge_used[rid] = 1;
      previous = rid;
    }
  }
  for (int t = 0; t < fx.brep.m_T.Count(); ++t) {
    if (old_seam_trims.count(t)) {
      TASK_CHECK(map.trim_images[t].empty());
      continue;
    }
    TASK_CHECK(!map.trim_images[t].empty());
    int previous = -1;
    for (int rid : map.trim_images[t]) {
      TASK_CHECK(rid >= 0 && rid < result.m_T.Count());
      TASK_CHECK(rid != previous);
      TASK_CHECK(trim_used[rid] == 0);
      trim_used[rid] = 1;
      previous = rid;
    }
  }

  std::set<int> seam_set(map.seam_edges.begin(), map.seam_edges.end());
  TASK_CHECK(seam_set.size() == map.seam_edges.size());
  std::set<int> topology_seam_edges;
  for (int li = 0; li < result.m_F[fx.face].m_li.Count(); ++li) {
    const ON_BrepLoop& loop = result.m_L[result.m_F[fx.face].m_li[li]];
    for (int k = 0; k < loop.m_ti.Count(); ++k) {
      const ON_BrepTrim& trim = result.m_T[loop.m_ti[k]];
      if (trim.m_type == ON_BrepTrim::seam)
        topology_seam_edges.insert(trim.m_ei);
    }
  }
  TASK_CHECK(topology_seam_edges == seam_set);
  for (int eid : map.seam_edges) {
    TASK_CHECK(eid >= 0 && eid < result.m_E.Count());
    const ON_BrepEdge& edge = result.m_E[eid];
    TASK_CHECK(edge.m_ti.Count() == 2);
    int seam_count = 0;
    for (int k = 0; k < edge.m_ti.Count(); ++k) {
      const ON_BrepTrim& trim = result.m_T[edge.m_ti[k]];
      TASK_CHECK(trim.m_type == ON_BrepTrim::seam);
      TASK_CHECK((trim.m_bRev3d ? trim.m_vi[0] == edge.m_vi[1]
                                : trim.m_vi[0] == edge.m_vi[0]));
      ++seam_count;
    }
    TASK_CHECK(seam_count == 2);
  }

  for (int e = 0; e < result.m_E.Count(); ++e) {
    if (seam_set.count(e))
      continue;
    TASK_CHECK(edge_used[e] == 1);
  }
  for (int t = 0; t < result.m_T.Count(); ++t) {
    if (result.m_T[t].m_type == ON_BrepTrim::seam)
      continue;
    TASK_CHECK(trim_used[t] == 1);
  }
  return 0;
}

int CheckGeometryAndMaterial(const Fixture& fx, const ON_Brep& result)
{
  const ON_NurbsSurface* old_surface =
    ON_NurbsSurface::Cast(fx.brep.m_S[fx.brep.m_F[0].m_si]);
  const ON_NurbsSurface* new_surface =
    ON_NurbsSurface::Cast(result.m_S[result.m_F[fx.face].m_si]);
  TASK_CHECK(old_surface != nullptr && new_surface != nullptr);
  const ON_Interval nd = new_surface->Domain(fx.dir);
  TASK_CHECK(std::fabs(nd[0] - fx.seam) <= DistEps(fx.seam, 1e-10));
  TASK_CHECK(std::fabs(nd[1] - (fx.seam + fx.period))
             <= DistEps(fx.seam + fx.period, 1e-10));

  for (int i = 0; i < 11; ++i) {
    const double dn = nd.ParameterAt((i + 0.5) / 11.0);
    for (int j = 0; j < 7; ++j) {
      const double o = fx.o0 + (fx.o1 - fx.o0) * (j + 0.5) / 7.0;
      double q = std::fmod(dn - fx.a, fx.period);
      if (q < 0.0)
        q += fx.period;
      const double old_d = fx.a + q;
      ON_3dPoint p_old, p_new;
      ON_3dVector du_old, dv_old, du_new, dv_new;
      TASK_CHECK(old_surface->Ev1Der(fx.dir == 0 ? old_d : o,
                                     fx.dir == 0 ? o : old_d,
                                     p_old, du_old, dv_old));
      TASK_CHECK(new_surface->Ev1Der(fx.dir == 0 ? dn : o,
                                     fx.dir == 0 ? o : dn,
                                     p_new, du_new, dv_new));
      TASK_CHECK(p_old.DistanceTo(p_new) <= 1.0e-8);
      TASK_CHECK((du_old - du_new).Length() <= 1.0e-8);
      TASK_CHECK((dv_old - dv_new).Length() <= 1.0e-8);
    }
  }

  const std::vector<UvSegment> segs = TargetSegments(result, fx.face, fx.dir);
  for (int i = 0; i < 24; ++i) {
    const double d = fx.seam + fx.period * (i + 0.37) / 24.0;
    for (int j = 0; j < 10; ++j) {
      const double o = fx.o0 + (fx.o1 - fx.o0) * (j + 0.61) / 10.0;
      TASK_CHECK(RayInside(segs, d, o) == ExpectedMaterial(fx, d, o));
    }
  }

  // Every edge/trim pair must evaluate to the same point at normalized t.
  for (int ei = 0; ei < result.m_E.Count(); ++ei) {
    const ON_BrepEdge& edge = result.m_E[ei];
    if (edge.m_edge_index < 0)
      continue;
    const ON_Interval ed = edge.Domain();
    for (int sample = 1; sample < 8; ++sample) {
      const double ne = sample / 8.0;
      const ON_3dPoint ep = edge.PointAt(ed.ParameterAt(ne));
      for (int k = 0; k < edge.m_ti.Count(); ++k) {
        const int ti = edge.m_ti[k];
        const ON_BrepTrim& trim = result.m_T[ti];
        const ON_Surface* surface = trim.SurfaceOf();
        TASK_CHECK(surface != nullptr);
        const double nt = trim.m_bRev3d ? 1.0 - ne : ne;
        const ON_Interval td = trim.Domain();
        const ON_3dPoint uv = trim.PointAt(td.ParameterAt(nt));
        const ON_3dPoint sp = surface->PointAt(uv.x, uv.y);
        TASK_CHECK(ep.DistanceTo(sp) <= 1.0e-8);
      }
    }
  }
  return 0;
}

int CheckSharedAdjacency(const Fixture& fx, const ON_Brep& result,
                         const ON_ReseamMap& map)
{
  TASK_CHECK(fx.shared_edge >= 0);
  const std::vector<int>& images = map.edge_images[fx.shared_edge];
  TASK_CHECK(images.size() == 2);
  for (int eid : images) {
    const ON_BrepEdge& edge = result.m_E[eid];
    TASK_CHECK(edge.m_ti.Count() == 2);
    int faces[2] = {-1, -1};
    for (int k = 0; k < 2; ++k) {
      const ON_BrepTrim& trim = result.m_T[edge.m_ti[k]];
      TASK_CHECK(trim.m_vi[0] == edge.m_vi[trim.m_bRev3d ? 1 : 0]);
      TASK_CHECK(trim.m_vi[1] == edge.m_vi[trim.m_bRev3d ? 0 : 1]);
      const ON_BrepLoop* loop = trim.Loop();
      TASK_CHECK(loop != nullptr);
      faces[k] = loop->m_fi;
    }
    TASK_CHECK(faces[0] != faces[1]);
  }
  return 0;
}

std::string Temp3dm(const char* name)
{
  const char* dir = std::getenv("TMPDIR");
  if (!dir || !*dir)
    dir = ".";
  return std::string(dir) + "/opennurbs_reseam_" + name + ".3dm";
}

int WriteAndRead(const ON_Brep& brep, const char* name, ON_Brep& loaded)
{
  const std::string path = Temp3dm(name);
  {
    ON_BinaryFile archive(ON::archive_mode::write3dm, path.c_str());
    TASK_CHECK(ON_WriteOneObjectArchive(archive, 0, brep));
  }
  ONX_Model model;
  TASK_CHECK(model.Read(path.c_str()));
  ONX_ModelComponentIterator components(model, ON_ModelComponent::Type::ModelGeometry);
  const ON_ModelGeometryComponent* component =
    ON_ModelGeometryComponent::Cast(components.FirstComponent());
  TASK_CHECK(component != nullptr);
  const ON_Brep* read_brep = ON_Brep::Cast(component->Geometry(nullptr));
  TASK_CHECK(read_brep != nullptr);
  loaded = *read_brep;
  TASK_CHECK(loaded.IsValid());
  return 0;
}

PHole CrossHole() { return {11.85, 0.65, -0.8, 0.8}; }
PHole SideHole() { return {13.6, 0.55, 1.1, 2.6}; }

int quotient_dir0(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.dir = 0;
  opt.holes = {CrossHole(), SideHole()};
  TASK_CHECK(BuildFixture(fx, opt));
  ON_Brep result;
  ON_ReseamMap map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, result, map));
  TASK_CHECK(CheckMappingShape(fx, result, map) == 0);
  TASK_CHECK(CheckGeometryAndMaterial(fx, result) == 0);
  return 0;
}

int quotient_dir1(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.dir = 1;
  // After transpose periodic y has domain [10,15]; centers are d values.
  opt.holes = {PHole{11.85, 0.65, -0.8, 0.8}, PHole{13.6, 0.55, 1.1, 2.6}};
  TASK_CHECK(BuildFixture(fx, opt));
  ON_Brep result;
  ON_ReseamMap map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 1, fx.seam, result, map));
  TASK_CHECK(CheckMappingShape(fx, result, map) == 0);
  TASK_CHECK(CheckGeometryAndMaterial(fx, result) == 0);
  return 0;
}

int quotient_reverse_proxy(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.reverse_proxy = true;
  opt.holes = {CrossHole(), SideHole()};
  TASK_CHECK(BuildFixture(fx, opt));
  ON_Brep result;
  ON_ReseamMap map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, result, map));
  TASK_CHECK(CheckMappingShape(fx, result, map) == 0);
  TASK_CHECK(CheckGeometryAndMaterial(fx, result) == 0);
  bool found_reversed = false;
  for (int e = 0; e < result.m_E.Count(); ++e) {
    if (result.m_E[e].ProxyCurveIsReversed())
      found_reversed = true;
  }
  TASK_CHECK(found_reversed);
  return 0;
}

int quotient_shared_surface_mesh(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.extra_surface_user = true;
  opt.holes = {CrossHole()};
  TASK_CHECK(BuildFixture(fx, opt));
  TASK_CHECK(fx.brep.m_F.Count() == 2);
  TASK_CHECK(fx.brep.m_F[1].m_si == fx.brep.m_F[0].m_si);
  TASK_CHECK(fx.brep.m_F[1].SharedMesh(ON::mesh_type::render_mesh) != nullptr);

  ON_Brep result;
  ON_ReseamMap map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, result, map));
  TASK_CHECK(CheckMappingShape(fx, result, map) == 0);
  TASK_CHECK(result.m_F[1].SharedMesh(ON::mesh_type::render_mesh) != nullptr);
  TASK_CHECK(result.m_F[1].m_si != result.m_F[0].m_si);
  const ON_Surface* shared_old = result.m_S[result.m_F[1].m_si];
  TASK_CHECK(shared_old != nullptr);
  const ON_Interval sd = shared_old->Domain(0);
  TASK_CHECK(std::fabs(sd[0] - fx.a) <= 1e-10);
  TASK_CHECK(std::fabs(sd[1] - fx.b) <= 1e-10);
  return 0;
}

int adjacency_shared_edge(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.adjacency = true;
  opt.holes = {CrossHole()};
  TASK_CHECK(BuildFixture(fx, opt));
  ON_Brep result;
  ON_ReseamMap map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, result, map));
  TASK_CHECK(CheckMappingShape(fx, result, map) == 0);
  TASK_CHECK(CheckGeometryAndMaterial(fx, result) == 0);
  TASK_CHECK(CheckSharedAdjacency(fx, result, map) == 0);

  ON_Brep loaded;
  TASK_CHECK(WriteAndRead(result, "adjacency", loaded) == 0);
  TASK_CHECK(loaded.IsValid());
  TASK_CHECK(CheckGeometryAndMaterial(fx, loaded) == 0);
  TASK_CHECK(CheckSharedAdjacency(fx, loaded, map) == 0);
  return 0;
}

int continuation_twice_and_back(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.holes = {CrossHole(), SideHole()};
  TASK_CHECK(BuildFixture(fx, opt));

  ON_Brep first;
  ON_ReseamMap map1;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, first, map1));
  TASK_CHECK(first.IsValid());

  const ON_Interval d1 = first.m_S[first.m_F[0].m_si]->Domain(0);
  const double seam2 = d1.ParameterAt(0.5);
  ON_Brep second;
  ON_ReseamMap map2;
  ON_TextLog second_log(stdout);
  TASK_CHECK(ON_ReseamBrepFace(first, 0, 0, seam2, second, map2, &second_log));
  TASK_CHECK(second.IsValid());

  // fx describes the original quotient.  The second result must still cover
  // the same periodic material, even though its cut moved twice.
  Fixture periodic = fx;
  const ON_Interval d2 = second.m_S[second.m_F[0].m_si]->Domain(0);
  periodic.seam = d2[0];
  TASK_CHECK(std::fabs(d2.Length() - fx.period) <= 1e-9);
  TASK_CHECK(CheckGeometryAndMaterial(periodic, second) == 0);

  // The old a/b seam locus is represented by b inside [fx.seam, fx.seam+P].
  ON_Brep back;
  ON_ReseamMap map_back;
  TASK_CHECK(ON_ReseamBrepFace(first, 0, 0, fx.b, back, map_back));
  TASK_CHECK(back.IsValid());
  const ON_Interval db = back.m_S[back.m_F[0].m_si]->Domain(0);
  TASK_CHECK(std::fabs(db[0] - fx.b) <= 1e-9);
  return 0;
}

int continuation_persistence_reedit(void)
{
  Fixture fx;
  FixtureOptions opt;
  opt.holes = {CrossHole()};
  TASK_CHECK(BuildFixture(fx, opt));
  ON_Brep first;
  ON_ReseamMap map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, first, map));
  ON_Brep loaded;
  TASK_CHECK(WriteAndRead(first, "reedit", loaded) == 0);

  ON_NurbsSurface* srf = ON_NurbsSurface::Cast(loaded.m_S[loaded.m_F[0].m_si]);
  TASK_CHECK(srf != nullptr);
  const double next_seam = srf->Domain(0).ParameterAt(0.71);
  ON_Brep edited;
  ON_ReseamMap map2;
  TASK_CHECK(ON_ReseamBrepFace(loaded, 0, 0, next_seam, edited, map2));
  TASK_CHECK(edited.IsValid());
  TASK_CHECK(edited.m_F.Count() == loaded.m_F.Count());
  return 0;
}

int continuation_failure_then_evaluate(void)
{
  Fixture fx;
  FixtureOptions bad_opt;
  bad_opt.holes = {CrossHole()};
  TASK_CHECK(BuildFixture(fx, bad_opt));

  ON_Brep preserved = fx.brep;
  ON_ReseamMap preserved_map;
  preserved_map.edge_images = {{11, 12}};
  preserved_map.trim_images = {{21}};
  preserved_map.seam_edges = {31};

  ON_Brep failed_result = fx.brep;
  ON_ReseamMap failed_map = preserved_map;
  TASK_CHECK(!ON_ReseamBrepFace(fx.brep, 99, 0, fx.seam, failed_result, failed_map));
  TASK_CHECK(failed_result.m_F.Count() == preserved.m_F.Count());
  TASK_CHECK(failed_map.edge_images.size() == 1);
  TASK_CHECK(failed_map.edge_images[0].size() == 2);
  TASK_CHECK(failed_map.trim_images[0].size() == 1);
  TASK_CHECK(failed_map.seam_edges.size() == 1);

  TASK_CHECK(!ON_ReseamBrepFace(fx.brep, 0, 7, fx.seam, failed_result, failed_map));
  TASK_CHECK(!ON_ReseamBrepFace(fx.brep, 0, 0, fx.a, failed_result, failed_map));
  TASK_CHECK(!ON_ReseamBrepFace(fx.brep, 0, 0, 11.2, failed_result, failed_map));
  TASK_CHECK(!ON_ReseamBrepFace(fx.brep, 0, 0, std::nan(""), failed_result, failed_map));

  // Aliasing is explicitly rejected without modifying the source object.
  ON_ReseamMap alias_map;
  TASK_CHECK(!ON_ReseamBrepFace(failed_result, 0, 0, fx.seam, failed_result, alias_map));
  TASK_CHECK(failed_result.IsValid());

  // The original object can still be evaluated after all failures.
  ON_Brep good;
  ON_ReseamMap good_map;
  TASK_CHECK(ON_ReseamBrepFace(fx.brep, 0, 0, fx.seam, good, good_map));
  TASK_CHECK(good.IsValid());
  return 0;
}

} // namespace

extern const struct task_case task_cases[] = {
  {"quotient", "dir0_multiple_holes", quotient_dir0},
  {"quotient", "dir1", quotient_dir1},
  {"quotient", "restricted_reverse_proxy", quotient_reverse_proxy},
  {"quotient", "shared_surface_and_mesh", quotient_shared_surface_mesh},
  {"adjacency", "shared_edge_split_propagation", adjacency_shared_edge},
  {"continuation", "twice_and_back_to_original_seam", continuation_twice_and_back},
  {"continuation", "persistence_then_reedit", continuation_persistence_reedit},
  {"continuation", "failure_preserves_and_continues", continuation_failure_then_evaluate},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
