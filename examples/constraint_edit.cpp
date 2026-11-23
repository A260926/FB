// Dynamic constraint editing example.
//
// Builds one surface patch, inserts a crease, partially removes it, then
// removes it completely, writing an SVG snapshot after each stage and an
// OBJ of the final mesh.  The parameter point (u,v) is lifted to
// (u, v, 0.1*u*v), reusing the exact same Point identities and triangle
// connectivity produced by the CDT.
//
// Usage: constraint_edit_example [output-directory]
// The directory defaults to the current working directory.

#include <poly2tri/poly2tri.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

const char* kSvgHeader =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"480\" height=\"420\" "
  "viewBox=\"-1 -1 12 11\" >\n";
const char* kSvgFooter = "</svg>\n";

double lift_z(double u, double v)
{
  return 0.1 * u * v;
}

void write_svg(const std::string& path, p2t::CDT& cdt)
{
  std::ofstream out(path);
  out << kSvgHeader;
  out << "<rect x=\"-1\" y=\"-1\" width=\"12\" height=\"11\" fill=\"#ffffff\"/>\n";

  // Draw every mesh edge once, keyed by its two endpoint pointers, so
  // boundary edges (a single incident triangle) are never dropped.
  struct EdgeKey {
    const p2t::Point* lo;
    const p2t::Point* hi;
    bool operator==(const EdgeKey& other) const
    {
      return lo == other.lo && hi == other.hi;
    }
  };
  struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& key) const
    {
      return std::hash<const p2t::Point*>{}(key.lo) ^
             (std::hash<const p2t::Point*>{}(key.hi) << 1);
    }
  };
  std::unordered_map<EdgeKey, bool, EdgeKeyHash> constrained;

  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      const p2t::Point* a = t->GetPoint(i);
      const p2t::Point* b = t->GetPoint((i + 1) % 3);
      const p2t::Point* lo = a < b ? a : b;
      const p2t::Point* hi = a < b ? b : a;
      // An edge is constrained if any incident triangle marks it so.
      constrained[{lo, hi}] =
        constrained.count({lo, hi}) != 0
          ? (constrained[{lo, hi}] || t->constrained_edge[i])
          : t->constrained_edge[i];
    }
  }

  for (const auto& entry : constrained) {
    const p2t::Point* a = entry.first.lo;
    const p2t::Point* b = entry.first.hi;
    const bool crease = entry.second;
    const char* color = crease ? "#c0392b" : "#9aa6b2";
    const double width = crease ? 0.10 : 0.035;
    out << "<line x1=\"" << a->x << "\" y1=\"" << 10.0 - a->y
        << "\" x2=\"" << b->x << "\" y2=\"" << 10.0 - b->y
        << "\" stroke=\"" << color << "\" stroke-width=\"" << width
        << "\" stroke-linecap=\"round\"/>\n";
  }

  // Vertices on top so the chain split is visible.
  std::set<const p2t::Point*> drawn;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      const p2t::Point* p = t->GetPoint(i);
      if (!drawn.insert(p).second) {
        continue;
      }
      out << "<circle cx=\"" << p->x << "\" cy=\"" << 10.0 - p->y
          << "\" r=\"0.13\" fill=\"#1c2b39\"/>\n";
    }
  }
  out << kSvgFooter;
}

void write_obj(const std::string& path, p2t::CDT& cdt)
{
  std::ofstream out(path);
  out << "# Surface patch exported with the same Point identities and\n"
      << "# triangle connectivity produced by p2t::CDT.\n";

  // Give every managed point a stable 1-based OBJ index via its pointer.
  std::vector<const p2t::Point*> points;
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    for (int i = 0; i < 3; ++i) {
      const p2t::Point* p = t->GetPoint(i);
      if (std::find(points.begin(), points.end(), p) == points.end()) {
        points.push_back(p);
      }
    }
  }
  for (const p2t::Point* p : points) {
    out << "v " << p->x << ' ' << p->y << ' ' << lift_z(p->x, p->y) << '\n';
  }
  out << '\n';
  for (p2t::Triangle* t : cdt.GetTriangles()) {
    out << 'f';
    for (int i = 0; i < 3; ++i) {
      const p2t::Point* p = t->GetPoint(i);
      const auto it = std::find(points.begin(), points.end(), p);
      const long index = std::distance(points.begin(), it) + 1;
      out << ' ' << index;
    }
    out << '\n';
  }
}

} // namespace

int main(int argc, char** argv)
{
  const std::string dir = argc > 1 ? std::string(argv[1]) : std::string(".");
  const auto join = [&](const std::string& name) {
    return dir + "/" + name;
  };

  // Same surface patch used by the baseline test: outer contour, one hole
  // and interior Steiner handles.
  std::vector<p2t::Point> outer{
    {0, 0}, {12, 0}, {12, 10}, {0, 10}};
  std::vector<p2t::Point> hole{
    {5, 4}, {5, 6}, {7, 6}, {7, 4}};
  std::vector<p2t::Point> handles{
    {2, 2}, {4, 2}, {8, 2}, {10, 2}, {2, 8}, {10, 8}};

  std::vector<p2t::Point*> outline;
  std::vector<p2t::Point*> holeline;
  for (p2t::Point& p : outer) {
    outline.push_back(&p);
  }
  for (p2t::Point& p : hole) {
    holeline.push_back(&p);
  }

  p2t::CDT cdt(outline);
  cdt.AddHole(holeline);
  for (p2t::Point& p : handles) {
    cdt.AddPoint(&p);
  }
  cdt.Triangulate();

  // Stage 1: mesh with the crease line added.
  const p2t::CDT::ConstraintId left =
    cdt.InsertConstraint(&handles[0], &handles[4]); // (2,2)-(2,8)
  const p2t::CDT::ConstraintId right =
    cdt.InsertConstraint(&handles[3], &handles[5]); // (10,2)-(10,8)
  if (left == 0 || right == 0) {
    std::fprintf(stderr, "failed to insert example crease lines\n");
    return 1;
  }
  write_svg(join("constraint_added.svg"), cdt);

  // Stage 2: one of the two creases removed (partial undo); the other and
  // all boundary edges stay protected.
  if (!cdt.RemoveConstraint(right)) {
    std::fprintf(stderr, "failed to remove example crease\n");
    return 1;
  }
  write_svg(join("constraint_partial.svg"), cdt);

  // Stage 3: every user constraint removed; the mesh is re-legalized.
  if (!cdt.RemoveConstraint(left)) {
    std::fprintf(stderr, "failed to remove remaining crease\n");
    return 1;
  }
  write_svg(join("constraint_removed.svg"), cdt);

  // OBJ of the final surface using the lifted mapping (u,v)->(u,v,0.1uv).
  write_obj(join("surface_patch.obj"), cdt);

  std::printf("wrote constraint_added.svg, constraint_partial.svg, "
              "constraint_removed.svg and surface_patch.obj in %s\n",
              dir.c_str());
  return 0;
}
