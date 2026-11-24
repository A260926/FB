#include "opennurbs_public.h"
#include "opennurbs_brep_reseam.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Segment { double u0, v0, u1, v1; };

void AddRectHole(ON_Brep& brep, int fi, double u0, double v0, double u1, double v1)
{
  ON_BrepFace& face = brep.m_F[fi];
  ON_Surface* surface = brep.m_S[face.m_si];
  ON_2dPoint uv[4] = {
    ON_2dPoint(u0, v0), ON_2dPoint(u0, v1),
    ON_2dPoint(u1, v1), ON_2dPoint(u1, v0)
  };

  int vid[4];
  for (int i = 0; i < 4; ++i) {
    vid[i] = brep.NewVertex(surface->PointAt(uv[i].x, uv[i].y)).m_vertex_index;
    brep.m_V[vid[i]].m_tolerance = 0.0;
  }

  ON_BrepLoop& loop = brep.NewLoop(ON_BrepLoop::inner, face);
  for (int i = 0; i < 4; ++i) {
    const int j = (i + 1) % 4;
    const bool varies_u = (i == 1 || i == 3);
    const double c0 = varies_u ? uv[i].x : uv[i].y;
    const double c1 = varies_u ? uv[j].x : uv[j].y;
    const double fixed = varies_u ? uv[i].y : uv[i].x;
    const ON_Interval seg(std::min(c0, c1), std::max(c0, c1));
    ON_Curve* iso = surface->IsoCurve(varies_u ? 0 : 1, fixed);
    const int c3i = brep.AddEdgeCurve(iso);
    const int ei = brep.NewEdge(c3i).m_edge_index;
    ON_BrepEdge& edge = brep.m_E[ei];
    edge.m_vi[0] = vid[i];
    edge.m_vi[1] = vid[j];
    brep.m_V[vid[i]].m_ei.Append(ei);
    brep.m_V[vid[j]].m_ei.Append(ei);
    edge.SetProxyCurve(brep.m_C3[c3i], seg);
    if (c0 > c1)
      edge.ON_CurveProxy::Reverse();
    edge.m_tolerance = 0.0;

    ON_LineCurve* c2 = new ON_LineCurve(uv[i], uv[j]);
    const int c2i = brep.AddTrimCurve(c2);
    ON_BrepTrim& trim = brep.NewTrim(edge, false, loop, c2i);
    trim.m_type = ON_BrepTrim::boundary;
    trim.m_tolerance[0] = trim.m_tolerance[1] = 0.0;
  }
}

std::vector<Segment> TargetSegments(const ON_Brep& brep)
{
  std::vector<Segment> segments;
  const ON_BrepFace& face = brep.m_F[0];
  for (int li = 0; li < face.m_li.Count(); ++li) {
    const ON_BrepLoop& loop = brep.m_L[face.m_li[li]];
    for (int k = 0; k < loop.m_ti.Count(); ++k) {
      const ON_BrepTrim& trim = brep.m_T[loop.m_ti[k]];
      const ON_Interval d = trim.Domain();
      const ON_3dPoint p0 = trim.PointAt(d[0]);
      const ON_3dPoint p1 = trim.PointAt(d[1]);
      segments.push_back({p0.x, p0.y, p1.x, p1.y});
    }
  }
  return segments;
}

bool RayInside(const std::vector<Segment>& segments, double u, double v)
{
  bool inside = false;
  for (const Segment& s : segments) {
    if (std::fabs(s.u0 - s.u1) > 1.0e-9)
      continue;
    if ((s.v0 > v) != (s.v1 > v) && s.u0 > u)
      inside = !inside;
  }
  return inside;
}

const ON_Brep* ReadOnlyBrep(ONX_Model& model, const char* path)
{
  if (!model.Read(path))
    return nullptr;
  ONX_ModelComponentIterator it(model, ON_ModelComponent::Type::ModelGeometry);
  const ON_ModelGeometryComponent* component =
    ON_ModelGeometryComponent::Cast(it.FirstComponent());
  if (!component)
    return nullptr;
  return ON_Brep::Cast(component->Geometry(nullptr));
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s output.3dm\n", argv[0]);
    return 2;
  }

  ON::Begin();

  // A rational degree-two closed cylinder surface with deliberately
  // non-unit parameter domains.
  ON_Cylinder cylinder(ON_Circle(ON_Plane::World_xy, 1.0), 2.0);
  ON_NurbsSurface nurbs;
  if (!cylinder.GetNurbForm(nurbs))
    return 3;
  nurbs.SetDomain(0, 10.0, 15.0);
  nurbs.SetDomain(1, -2.0, 4.0);

  ON_Brep source;
  if (!source.NewFace(nurbs))
    return 4;

  // This rectangular hole is crossed by the selected new seam u=12.
  AddRectHole(source, 0, 11.35, -0.55, 12.65, 0.65);
  source.SetTrimIsoFlags();
  source.SetTrimTypeFlags(false);
  source.Compact();
  if (!source.IsValid())
    return 5;

  const double seam = 12.0;
  ON_Brep resewn;
  ON_ReseamMap map;
  ON_TextLog log(stdout);
  if (!ON_ReseamBrepFace(source, 0, 0, seam, resewn, map, &log))
    return 6;
  if (!resewn.IsValid() || map.seam_edges.size() != 2)
    return 7;

  // Periodic UV material check: the center of the seam-crossing hole is void.
  const ON_Interval new_u = resewn.m_S[resewn.m_F[0].m_si]->Domain(0);
  if (std::fabs(new_u[0] - seam) > 1.0e-10 || std::fabs(new_u.Length() - 5.0) > 1.0e-10)
    return 8;
  const auto segments = TargetSegments(resewn);
  if (RayInside(segments, seam, 0.05))
    return 9;
  if (!RayInside(segments, seam + 1.5, -1.4))
    return 10;

  {
    ON_BinaryFile archive(ON::archive_mode::write3dm, argv[1]);
    if (!ON_WriteOneObjectArchive(archive, 0, resewn))
      return 11;
  }

  ONX_Model model;
  const ON_Brep* loaded = ReadOnlyBrep(model, argv[1]);
  if (!loaded || !loaded->IsValid() || loaded->m_F.Count() != 1)
    return 12;

  // The persisted object can be edited again.
  ON_NurbsSurface* loaded_surface =
    ON_NurbsSurface::Cast(loaded->m_S[loaded->m_F[0].m_si]);
  if (!loaded_surface)
    return 13;
  const double next_seam = loaded_surface->Domain(0).ParameterAt(0.72);
  ON_Brep reedited;
  ON_ReseamMap second_map;
  if (!ON_ReseamBrepFace(*loaded, 0, 0, next_seam, reedited, second_map))
    return 14;
  if (!reedited.IsValid() || reedited.m_F.Count() != 1)
    return 15;

  std::printf("reseam example wrote and verified %s (seam edges=%zu)\n",
              argv[1], map.seam_edges.size());
  ON::End();
  return 0;
}
