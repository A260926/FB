// Dynamic internal constraints on an existing Poly2Tri mesh.
//
// The surface editor maps a Poly2Tri triangulation of a planar parameter
// domain (u, v) into 3D.  Outer contour and holes define the surface border,
// interior control points carry vertex attributes.  This example adds, partly
// removes and fully removes crease lines after the single Triangulate() call,
// and writes the three meshes as SVG.  The same vertex objects and the same
// triangle connectivity are then lifted to (u, v, 0.1*u*v) and exported as an
// OBJ surface patch, showing that an active crease is a real mesh edge.

#include <poly2tri/poly2tri.h>

#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using p2t::CDT;
using p2t::Point;
using p2t::Triangle;

struct EdgeKey {
  const Point* p;
  const Point* q;

  bool operator<(const EdgeKey& other) const
  {
    if (p != other.p)
      return p < other.p;
    return q < other.q;
  }
  bool operator==(const EdgeKey& other) const { return p == other.p && q == other.q; }
};

using EdgeSet = std::set<EdgeKey>;

EdgeKey Key(const Point* a, const Point* b)
{
  return (a < b) ? EdgeKey{a, b} : EdgeKey{b, a};
}

struct Surface {
  std::vector<std::unique_ptr<Point>> owned;
  std::vector<Point*> outer;
  std::vector<Point*> hole;
  std::vector<Point*> interior;
  std::unique_ptr<CDT> cdt;

  Point* Add(double u, double v)
  {
    owned.emplace_back(new Point(u, v));
    return owned.back().get();
  }

  Surface()
  {
    // Outer contour: an L-shaped patch, so crease segments have to respect a
    // concave border (a straight line between two legal handles can still
    // leave the filled region through the notch).
    outer.push_back(Add(0, 0));
    outer.push_back(Add(120, 0));
    outer.push_back(Add(120, 80));
    outer.push_back(Add(80, 80));
    outer.push_back(Add(80, 120));
    outer.push_back(Add(0, 120));

    // A square hole in the upper arm, away from every crease below.
    hole.push_back(Add(5, 85));
    hole.push_back(Add(5, 115));
    hole.push_back(Add(40, 115));
    hole.push_back(Add(40, 85));

    // Interior handles: three full rows in the lower band plus a small block
    // in the upper arm.  The long crease below runs through the middle row.
    for (int iv = 0; iv < 3; ++iv) {
      for (int iu = 0; iu < 4; ++iu)
        interior.push_back(Add(15.0 + 25.0 * iu, 15.0 + 25.0 * iv));
    }
    interior.push_back(Add(50, 90));
    interior.push_back(Add(65, 90));
    interior.push_back(Add(50, 105));
    interior.push_back(Add(65, 105));

    // Two off-grid handles for the main diagonal crease.
    main_a = Add(20, 60);
    main_b = Add(110, 20);

    cdt.reset(new CDT(outer));
    cdt->AddHole(hole);
    for (Point* p : interior)
      cdt->AddPoint(p);
    cdt->AddPoint(main_a);
    cdt->AddPoint(main_b);
    cdt->Triangulate();
  }

  std::vector<Triangle*> Tris() { return cdt->GetTriangles(); }

  /// Grid handle of the lower band: row 0 is y=15 and row 2 is y=65.
  Point* Grid(int iu, int iv) { return interior[static_cast<size_t>(iv * 4 + iu)]; }

  Point* main_a = nullptr;
  Point* main_b = nullptr;
};

int VertexIndex(const std::vector<Point*>& order, const Point* p)
{
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i] == p)
      return static_cast<int>(i);
  }
  return -1;
}

std::vector<Point*> VertexOrder(const std::vector<Triangle*>& tris)
{
  std::vector<Point*> order;
  std::set<const Point*> seen;
  for (Triangle* t : tris) {
    for (int i = 0; i < 3; ++i) {
      Point* p = t->GetPoint(i);
      if (seen.insert(p).second)
        order.push_back(p);
    }
  }
  return order;
}

void WriteSvg(const std::string& path, const std::vector<Triangle*>& tris,
              const std::vector<Point*>& input_points)
{
  // Fit the parameter box into the canvas with a margin, flipping v so the
  // SVG looks like the parameter plane.
  const double width = 640.0;
  const double height = 640.0;
  const double margin = 30.0;
  double min_u = 0.0, max_u = 120.0, min_v = 0.0, max_v = 120.0;
  const double scale = std::min((width - 2 * margin) / (max_u - min_u),
                                (height - 2 * margin) / (max_v - min_v));
  auto sx = [&](double u) { return margin + (u - min_u) * scale; };
  auto sy = [&](double v) { return height - margin - (v - min_v) * scale; };

  std::ofstream out(path.c_str());
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\""
      << height << "\" viewBox=\"0 0 " << width << " " << height << "\">\n";
  out << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n";

  // Ordinary edges in pale grey, protected crease edges in a strong colour.
  for (Triangle* t : tris) {
    for (int i = 0; i < 3; ++i) {
      const Point* a = t->GetPoint((i + 1) % 3);
      const Point* b = t->GetPoint((i + 2) % 3);
      const bool crease = t->constrained_edge[i];
      out << "<line x1=\"" << sx(a->x) << "\" y1=\"" << sy(a->y) << "\" x2=\"" << sx(b->x)
          << "\" y2=\"" << sy(b->y) << "\" stroke=\"" << (crease ? "#c0392b" : "#c9cdd2")
          << "\" stroke-width=\"" << (crease ? 2.6 : 0.9) << "\"/>\n";
    }
  }
  // Vertices: input control points get a dot, the edits never invent any.
  for (const Point* p : input_points) {
    out << "<circle cx=\"" << sx(p->x) << "\" cy=\"" << sy(p->y) << "\" r=\"1.7\" fill=\"#2c3e50\"/>\n";
  }
  out << "</svg>\n";
}

bool EdgeActive(const std::vector<Triangle*>& tris, const Point* a, const Point* b)
{
  for (Triangle* t : tris) {
    const int index = t->EdgeIndex(a, b);
    if (index >= 0 && t->constrained_edge[index])
      return true;
  }
  return false;
}

/// Export the current mesh as an OBJ surface patch.  Parameter coordinates
/// (u, v) map to (u, v, 0.1 * u * v).  Only the crease sub-edges that are
/// actual triangle sides are emitted as `l` records, so downstream tools see
/// the same connectivity as the triangles.
void WriteObj(const std::string& path, const std::vector<Triangle*>& tris,
              const EdgeSet& base_edges)
{
  const std::vector<Point*> order = VertexOrder(tris);

  std::ofstream out(path.c_str());
  out << "# poly2tri parameter patch lifted to (u, v, 0.1*u*v)\n";
  for (const Point* p : order) {
    const double u = p->x;
    const double v = p->y;
    out << "v " << u << " " << v << " " << (0.1 * u * v) << "\n";
  }
  for (Triangle* t : tris) {
    int index[3];
    for (int i = 0; i < 3; ++i)
      index[i] = VertexIndex(order, t->GetPoint(i)) + 1;
    out << "f " << index[0] << " " << index[1] << " " << index[2] << "\n";
  }

  // Emit each newly forced crease edge exactly once, but only when it really
  // is a side of the triangulation (never a long edge that skipped vertices).
  std::set<EdgeKey> creases;
  for (Triangle* t : tris) {
    for (int i = 0; i < 3; ++i) {
      if (!t->constrained_edge[i])
        continue;
      const Point* a = t->GetPoint((i + 1) % 3);
      const Point* b = t->GetPoint((i + 2) % 3);
      const EdgeKey key = Key(a, b);
      if (base_edges.count(key) || !creases.insert(key).second)
        continue;
      out << "l " << (VertexIndex(order, key.p) + 1) << " " << (VertexIndex(order, key.q) + 1)
          << "\n";
    }
  }
}

void Report(const char* tag, const std::vector<Triangle*>& tris)
{
  double area = 0.0;
  size_t creases = 0;
  for (Triangle* t : tris) {
    const Point* a = t->GetPoint(0);
    const Point* b = t->GetPoint(1);
    const Point* c = t->GetPoint(2);
    area += std::abs((b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x)) * 0.5;
    for (int i = 0; i < 3; ++i) {
      if (t->constrained_edge[i])
        ++creases;
    }
  }
  std::cout << tag << " triangles=" << tris.size() << " area=" << area
            << " constrained_flags=" << creases << "\n";
}

} // namespace

int main(int argc, char** argv)
{
  const std::string prefix = argc > 1 ? argv[1] : "constraint_edit";

  Surface surface;
  const std::vector<Point*> inputs = [&surface] {
    std::vector<Point*> all = surface.outer;
    all.insert(all.end(), surface.hole.begin(), surface.hole.end());
    all.insert(all.end(), surface.interior.begin(), surface.interior.end());
    return all;
  }();

  Report("initial", surface.Tris());
  WriteSvg(prefix + "_initial.svg", surface.Tris(), inputs);
  const EdgeSet base_edges = [&surface] {
    EdgeSet set;
    for (Triangle* t : surface.Tris()) {
      for (int i = 0; i < 3; ++i) {
        if (t->constrained_edge[i])
          set.insert(Key(t->GetPoint((i + 1) % 3), t->GetPoint((i + 2) % 3)));
      }
    }
    return set;
  }();

  // Three crease lines.  The main diagonal crosses many triangles, and the
  // long grid line runs through two existing handles, so it is split into
  // real sub-edges instead of one long side.
  const CDT::ConstraintId main_crease =
      surface.cdt->InsertConstraint(surface.main_a, surface.main_b);
  const CDT::ConstraintId long_crease =
      surface.cdt->InsertConstraint(surface.Grid(0, 2), surface.Grid(3, 2));
  const CDT::ConstraintId short_crease =
      surface.cdt->InsertConstraint(surface.Grid(1, 2), surface.Grid(2, 2));
  if (main_crease == 0 || long_crease == 0 || short_crease == 0) {
    std::cerr << "crease insertion failed\n";
    return 1;
  }
  Report("with_creases", surface.Tris());
  WriteSvg(prefix + "_creases.svg", surface.Tris(), inputs);
  WriteObj(prefix + "_creases.obj", surface.Tris(), base_edges);

  // Partially undo: the long crease owns three sub-edges, two of which are
  // also shared with the still-active short crease.  Dropping the long id must
  // release only its own contribution: the shared middle sub-edge stays
  // protected, the two outer ones become ordinary again.
  if (!surface.cdt->RemoveConstraint(long_crease)) {
    std::cerr << "partial removal failed\n";
    return 1;
  }
  const bool outer_released = !EdgeActive(surface.Tris(), surface.Grid(0, 2),
                                          surface.Grid(1, 2));
  const bool shared_still_forced =
      EdgeActive(surface.Tris(), surface.Grid(1, 2), surface.Grid(2, 2));
  std::cout << "after_partial_remove outer_subedge_released=" << (outer_released ? 1 : 0)
            << " shared_subedge_still_forced=" << (shared_still_forced ? 1 : 0) << "\n";
  Report("partial", surface.Tris());
  WriteSvg(prefix + "_partial.svg", surface.Tris(), inputs);

  // Full undo of the remaining creases: the mesh re-legalises locally.
  if (!surface.cdt->RemoveConstraint(short_crease) ||
      !surface.cdt->RemoveConstraint(main_crease)) {
    std::cerr << "final removal failed\n";
    return 1;
  }
  Report("removed", surface.Tris());
  WriteSvg(prefix + "_removed.svg", surface.Tris(), inputs);
  WriteObj(prefix + "_removed.obj", surface.Tris(), base_edges);

  std::cout << "wrote " << prefix << "_{initial,creases,partial,removed}.svg\n";
  std::cout << "wrote " << prefix << "_{creases,removed}.obj\n";
  return 0;
}
