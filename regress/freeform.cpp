// regress/freeform.cpp
//
// Regression tests for restricted B-spline free-form curve support in
// tinyobjloader: parsing through BOTH ObjReader entry points (string and
// temp file) and the streaming LoadObj container API, and geometry produced
// by TessellateCurve.
//
// The geometry checks never use the library's own sampling as a reference.
// Rational arcs are checked against their closed-form circular-arc formula,
// and every other curve against an independent Cox-de Boor basis-function
// evaluation written from scratch below. The program prints:
//
//   PASS source-state cases=N
//   PASS rational-geometry cases=N
//   PASS discontinuities cases=N
//   PASS invalid-blocks cases=N
//   TINYOBJ_FREEFORM_OK
//
// and exits non-zero on the first failed expectation.
//
// C++11. Builds with real_t as float or double.

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_source_cases = 0;
int g_geom_cases = 0;
int g_disc_cases = 0;
int g_invalid_cases = 0;

#define CHECK(cond, msg)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,  \
                   __LINE__, msg);                                    \
      ++g_failures;                                                   \
    }                                                                 \
  } while (0)

// ---------------------------------------------------------------------------
// Independent numerical reference (deliberately separate from the library's
// Boehm/de Casteljau implementation): Cox-de Boor basis recursion.
// ---------------------------------------------------------------------------

struct RefPoint {
  double x, y, z;
};

double basis_function(int i, int p, const std::vector<double> &U, double u) {
  if (p == 0) {
    // The last knot interval is closed at its right end.
    if (i + 1 == static_cast<int>(U.size()) - 1)
      return (u >= U[i] && u <= U[i + 1]) ? 1.0 : 0.0;
    return (u >= U[i] && u < U[i + 1]) ? 1.0 : 0.0;
  }
  double left = 0.0, right = 0.0;
  double d1 = U[i + p] - U[i];
  if (d1 != 0.0) left = (u - U[i]) / d1 * basis_function(i, p - 1, U, u);
  double d2 = U[i + p + 1] - U[i + 1];
  if (d2 != 0.0)
    right = (U[i + p + 1] - u) / d2 *
            basis_function(i + 1, p - 1, U, u);
  return left + right;
}

// Evaluate a (possibly rational) clamped B-spline at parameter u.
// `weights` has one entry per control point (1 for non-rational curves).
// `side` matters only at break knots: -1 returns the left one-sided limit,
// +1 the right one-sided limit, 0 the exact value.
RefPoint ref_evaluate_sided(const std::vector<RefPoint> &cp,
                            const std::vector<double> &weights,
                            const std::vector<double> &knots, int degree,
                            double u, int side) {
  if (side != 0) {
    // Evaluate just inside the neighboring non-empty knot interval to get
    // the one-sided limit of a (possibly discontinuous) curve at a knot.
    double scale = std::fabs(u) > 1.0 ? std::fabs(u) : 1.0;
    double delta = 1e-10 * scale;
    u += side < 0 ? -delta : delta;
    if (u < knots.front()) u = knots.front();
    if (u > knots.back()) u = knots.back();
  }
  const int n = static_cast<int>(cp.size());
  double den = 0.0;
  RefPoint acc = {0.0, 0.0, 0.0};
  for (int i = 0; i < n; ++i) {
    double N = basis_function(i, degree, knots, u);
    double wn = weights[i] * N;
    den += wn;
    acc.x += wn * cp[i].x;
    acc.y += wn * cp[i].y;
    acc.z += wn * cp[i].z;
  }
  acc.x /= den;
  acc.y /= den;
  acc.z /= den;
  return acc;
}

RefPoint ref_evaluate(const std::vector<RefPoint> &cp,
                      const std::vector<double> &weights,
                      const std::vector<double> &knots, int degree,
                      double u) {
  return ref_evaluate_sided(cp, weights, knots, degree, u, 0);
}

double distance3(const RefPoint &a, const RefPoint &b) {
  double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double point_segment_distance(const RefPoint &q, const RefPoint &a,
                               const RefPoint &b) {
  double vx = b.x - a.x, vy = b.y - a.y, vz = b.z - a.z;
  double len2 = vx * vx + vy * vy + vz * vz;
  if (len2 == 0.0) return distance3(q, a);
  double t = ((q.x - a.x) * vx + (q.y - a.y) * vy +
              (q.z - a.z) * vz) /
             len2;
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  RefPoint c = {a.x + t * vx, a.y + t * vy, a.z + t * vz};
  return distance3(q, c);
}

// Pull control points/weights of a parsed curve back out of the attribute
// arrays in double precision, for the independent reference evaluator.
void curve_reference_data(const tinyobj::attrib_t &attrib,
                          const tinyobj::curve_t &curve,
                          std::vector<RefPoint> *cp,
                          std::vector<double> *weights) {
  for (size_t i = 0; i < curve.indices.size(); ++i) {
    unsigned int vi = curve.indices[i];
    RefPoint p = {attrib.vertices[3 * vi + 0],
                  attrib.vertices[3 * vi + 1],
                  attrib.vertices[3 * vi + 2]};
    cp->push_back(p);
    double w = 1.0;
    if (curve.rational && vi < attrib.vertex_weights.size() &&
        vi < attrib.vertex_components.size() &&
        attrib.vertex_components[vi] == 4)
      w = attrib.vertex_weights[vi];
    weights->push_back(w);
  }
}

// Independent verification of the tolerance contract: on every output
// segment, densely sample the reference curve across the segment's parameter
// span and require each reference point to lie within `tolerance` of the
// emitted chord.
bool verify_segments_within_tolerance(
    const tinyobj::attrib_t &attrib, const tinyobj::curve_t &curve,
    const tinyobj::curve_polyline_t &line, double tolerance) {
  std::vector<RefPoint> cp;
  std::vector<double> weights;
  curve_reference_data(attrib, curve, &cp, &weights);
  std::vector<double> knots(curve.knots.begin(), curve.knots.end());
  auto is_break_knot = [&knots](double u) {
    for (size_t i = 0; i + 1 < knots.size(); ++i)
      if (knots[i] == u && knots[i + 1] == u) return true;
    return false;
  };
  const double slack = tolerance * 1e-6 + 1e-12;
  const int kDense = 40;
  for (size_t s = 0; s + 1 < line.samples.size(); ++s) {
    double u0 = line.samples[s].param;
    double u1 = line.samples[s + 1].param;
    if (!(u1 > u0)) return false;
    RefPoint a = {line.samples[s].position[0],
                  line.samples[s].position[1],
                  line.samples[s].position[2]};
    RefPoint b = {line.samples[s + 1].position[0],
                  line.samples[s + 1].position[1],
                  line.samples[s + 1].position[2]};
    for (int k = 0; k <= kDense; ++k) {
      double u = u0 + (u1 - u0) * (static_cast<double>(k) / kDense);
      // At a break knot the two runs hold one-sided limits; evaluate the
      // reference on the matching side instead of its right-continuous value.
      int side = 0;
      if (k == 0 && is_break_knot(u)) side = +1;
      if (k == kDense && is_break_knot(u)) side = -1;
      RefPoint ref =
          ref_evaluate_sided(cp, weights, knots, curve.degree, u, side);
      if (point_segment_distance(ref, a, b) > tolerance + slack) return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Three-path parsing: string ObjReader, temp-file ObjReader, streaming LoadObj
// ---------------------------------------------------------------------------

std::string make_temp_path(const char *suffix) {
  const char *dir = std::getenv("TMPDIR");
  std::string base = (dir && *dir) ? dir : ".";
  static int counter = 0;
  std::ostringstream ss;
  ss << base << "/tinyobj_freeform_" << static_cast<long>(std::rand()) << "_"
     << counter++ << suffix;
  return ss.str();
}

bool write_temp_file(const std::string &text, std::string *path) {
  *path = make_temp_path(".obj");
  std::ofstream ofs(path->c_str());
  if (!ofs) return false;
  ofs << text;
  ofs.close();
  return static_cast<bool>(ofs);
}

bool error_has_line_number(const std::string &e) {
  // clang-style "<stream>:LINE:COL: error:" or "file:LINE:COL: error:"
  for (size_t i = 0; i + 3 < e.size(); ++i)
    if (e[i] == ':' && e[i + 1] >= '0' && e[i + 1] <= '9') return true;
  return false;
}

bool same_curve(const tinyobj::curve_t &a, const tinyobj::curve_t &b) {
  if (a.degree != b.degree || a.rational != b.rational ||
      a.param_begin != b.param_begin || a.param_end != b.param_end ||
      a.material_id != b.material_id || a.indices.size() != b.indices.size() ||
      a.knots.size() != b.knots.size())
    return false;
  for (size_t i = 0; i < a.indices.size(); ++i)
    if (a.indices[i] != b.indices[i]) return false;
  for (size_t i = 0; i < a.knots.size(); ++i)
    if (a.knots[i] != b.knots[i]) return false;
  return true;
}

struct Paths {
  tinyobj::ObjReader string_reader;
  tinyobj::ObjReader file_reader;
  tinyobj::attrib_t stream_attrib;
  std::vector<tinyobj::shape_t> stream_shapes;
  std::vector<tinyobj::material_t> stream_materials;
  bool string_ok;
  bool file_ok;
  bool stream_ok;
  std::string stream_err;

  Paths() : string_ok(false), file_ok(false), stream_ok(false) {}

  bool all_ok() const { return string_ok && file_ok && stream_ok; }
  const tinyobj::attrib_t &attrib() const {
    return string_reader.GetAttrib();
  }
  const std::vector<tinyobj::shape_t> &shapes() const {
    return string_reader.GetShapes();
  }
};

// Parse through all three container entry points.
Paths parse3(const std::string &obj_text, const std::string &mtl_text = "") {
  Paths pp;
  pp.string_ok = pp.string_reader.ParseFromString(obj_text, mtl_text);

  std::string path;
  if (write_temp_file(obj_text, &path)) {
    pp.file_ok = pp.file_reader.ParseFromFile(path);
    std::remove(path.c_str());
  }

  std::istringstream iss(obj_text);
  std::string warn;
  pp.stream_ok = tinyobj::LoadObj(&pp.stream_attrib, &pp.stream_shapes,
                                  &pp.stream_materials, &warn, &pp.stream_err,
                                  &iss, NULL, true, true);
  return pp;
}

// All three paths must reject, each with an actual line number.
bool reject3(const std::string &obj_text) {
  Paths pp = parse3(obj_text);
  if (pp.string_ok || pp.file_ok || pp.stream_ok) return false;
  return error_has_line_number(pp.string_reader.Error()) &&
         error_has_line_number(pp.file_reader.Error()) &&
         error_has_line_number(pp.stream_err);
}

// Cross-equivalence check used after every successful parse3.
bool paths_agree(const Paths &pp) {
  const auto &a = pp.string_reader.GetShapes();
  const auto &b = pp.file_reader.GetShapes();
  const auto &c = pp.stream_shapes;
  if (a.size() != b.size() || a.size() != c.size()) return false;
  size_t total_curves = 0;
  for (size_t s = 0; s < a.size(); ++s) {
    if (a[s].curves.size() != b[s].curves.size() ||
        a[s].curves.size() != c[s].curves.size())
      return false;
    if (a[s].name != b[s].name || a[s].name != c[s].name) return false;
    for (size_t k = 0; k < a[s].curves.size(); ++k) {
      if (!same_curve(a[s].curves[k], b[s].curves[k]) ||
          !same_curve(a[s].curves[k], c[s].curves[k]))
        return false;
      ++total_curves;
    }
  }
  if (total_curves == 0) return false;
  const tinyobj::attrib_t &sa = pp.string_reader.GetAttrib();
  const tinyobj::attrib_t &fa = pp.file_reader.GetAttrib();
  if (sa.vertices != fa.vertices ||
      sa.vertices != pp.stream_attrib.vertices ||
      sa.vertex_weights != fa.vertex_weights ||
      sa.vertex_weights != pp.stream_attrib.vertex_weights ||
      sa.vertex_components != fa.vertex_components ||
      sa.vertex_components != pp.stream_attrib.vertex_components ||
      sa.colors != fa.colors || sa.colors != pp.stream_attrib.colors)
    return false;
  return true;
}

// The quarter-circle OBJ used across several groups: rational quadratic with
// weights 1, sqrt(1/2), 1.
const char *kQuarterCircleObj =
    "v 1 0 0 1\n"
    "v 1 1 0 0.7071067811865475244\n"
    "v 0 1 0 1\n"
    "cstype rat bspline\n"
    "deg 2\n"
    "curv 0 1 1 2 3\n"
    "parm u 0 0 0\n"
    "parm u 1 1 1\n"
    "end\n";

// ===========================================================================
// Group 1: parsing and source state
// ===========================================================================

void test_source_state() {
  // --- 1. All three parse paths agree on the parsed curve fields.
  {
    Paths pp = parse3(kQuarterCircleObj);
    CHECK(pp.all_ok(), "parse3 quarter circle");
    ++g_source_cases;
    CHECK(paths_agree(pp), "three paths produce identical curve data");
    ++g_source_cases;

    CHECK(pp.shapes().size() == 1 && pp.shapes()[0].curves.size() == 1,
          "exactly one shape and curve");
    const tinyobj::curve_t &a = pp.shapes()[0].curves[0];
    CHECK(a.degree == 2 && a.rational, "degree 2 rational");
    CHECK(a.indices.size() == 3 && a.indices[0] == 0 &&
              a.indices[1] == 1 && a.indices[2] == 2,
          "zero-based resolved indices");
    CHECK(a.knots.size() == 6, "knot count n+p+1");
    CHECK(a.param_begin == 0.0 && a.param_end == 1.0, "curv parameter range");
    CHECK(a.material_id == -1, "no material -> -1");
    ++g_source_cases;

    bool knots_ok = true;
    const double expect_knots[6] = {0, 0, 0, 1, 1, 1};
    for (int i = 0; i < 6; ++i)
      knots_ok = knots_ok && a.knots[i] == expect_knots[i];
    CHECK(knots_ok, "multi-line parm u accumulation");
    ++g_source_cases;

    CHECK(pp.attrib().vertex_components.size() == 3 &&
              pp.attrib().vertex_components[0] == 4 &&
              pp.attrib().vertex_components[1] == 4 &&
              pp.attrib().vertex_components[2] == 4,
          "xyzw component tracking");
    ++g_source_cases;
  }

  // --- 2. Negative indices resolve against the vertices present at the curv
  // line; vertices added afterwards must not change the resolved sources.
  {
    const char *obj =
        "v 10 0 0\n"
        "v 11 0 0\n"
        "v 12 0 0\n"
        "cstype bspline\n"
        "deg 1\n"
        "curv 0 1 -2 -1\n"       // resolves to vertices 1 and 2
        "parm u 0 0 1 1\n"
        "end\n"
        "v 13 0 0\n"
        "v 14 0 0\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "negative index parse3");
    ++g_source_cases;
    CHECK(paths_agree(pp), "negative index three-path agreement");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    CHECK(c.indices.size() == 2 && c.indices[0] == 1 &&
              c.indices[1] == 2,
          "negative indices frozen at curv line vertex count");
    CHECK(pp.attrib().vertices.size() == 15, "all five vertices present");
    ++g_source_cases;
  }

  // --- 3. Object/group switches must not drop curve-only shapes.
  {
    const char *obj =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "o first\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n"
        "o second\n"
        "v 3 3 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 4\nparm u 0 0 0 1 1 1\nend\n"
        "g grp\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 3 4\nparm u 0 0 0 1 1 1\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "object switch parse3");
    ++g_source_cases;
    CHECK(paths_agree(pp), "object switch three-path agreement");
    const auto &shapes = pp.shapes();
    CHECK(shapes.size() == 3, "three curve-only shapes kept across o/g");
    if (shapes.size() == 3) {
      CHECK(shapes[0].curves.size() == 1 && shapes[1].curves.size() == 1 &&
                shapes[2].curves.size() == 1,
            "each shape carries its curve");
      CHECK(shapes[0].name == "first" && shapes[1].name == "second",
            "object names preserved");
      CHECK(shapes[2].curves[0].indices[2] == 3,
            "later curve sees later vertices");
    }
    ++g_source_cases;
  }

  // --- 4. Six-component vertex colors are not mistaken for curve weights.
  {
    const char *obj =
        "v 0 0 0 1.0 0.0 0.0\n"
        "v 1 1 0 0.0 1.0 0.0\n"
        "v 2 0 0 0.0 0.0 1.0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "color vertex non-rational parse3");
    ++g_source_cases;
    CHECK(paths_agree(pp), "color vertex three-path agreement");
    const auto &a = pp.attrib();
    CHECK(a.vertex_components.size() == 3 &&
              a.vertex_components[0] == 6 && a.vertex_components[1] == 6,
          "six-component vertices marked as colors");
    CHECK(a.colors.size() == 9 && a.colors[3] == 0.0 &&
              a.colors[4] == 1.0,
          "colors stored in color array");
    CHECK(!pp.shapes()[0].curves[0].rational,
          "non-rational curve built from color vertices");
    ++g_source_cases;

    const char *rat =
        "v 0 0 0 1.0 0.0 0.0\n"
        "v 1 1 0 0.0 1.0 0.0\n"
        "v 2 0 0 0.0 0.0 1.0\n"
        "cstype rat bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(rat), "rational + color vertex rejected on all paths");
    tinyobj::ObjReader probe;
    probe.ParseFromString(rat, "");
    CHECK(probe.Error().find("6-component") != std::string::npos,
          "color rejection explains why");
    ++g_source_cases;
  }

  // --- 5. Material active at `end` is recorded on the curve. The string API
  // ignores `mtllib`, so this case is driven through a temp file.
  {
    const char *obj =
        "mtllib red.mtl\n"
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "usemtl red\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    const char *dir = std::getenv("TMPDIR");
    std::string base = (dir && *dir) ? dir : ".";
    std::string mtl_path = base + "/red.mtl";
    std::ofstream mfs(mtl_path.c_str());
    mfs << "newmtl red\nKd 1 0 0\n";
    mfs.close();
    std::string obj_path;
    bool wrote = write_temp_file(obj, &obj_path);
    tinyobj::ObjReader r;
    bool ok = wrote && r.ParseFromFile(obj_path);
    if (wrote) std::remove(obj_path.c_str());
    std::remove(mtl_path.c_str());
    CHECK(ok, "usemtl curve parse from file");
    ++g_source_cases;
    CHECK(r.GetMaterials().size() == 1 &&
              r.GetShapes()[0].curves[0].material_id == 0,
          "curve stores material id at end");
    ++g_source_cases;
  }

  // --- 6. Unknown free-form element types stay ignored, and mixed face /
  // color input from the old behavior still parses.
  {
    const char *obj =
        "v 0 0 0 1 1 1\nv 1 0 0 0 0 0\nv 1 1 0 0.5 0.5 0.5\nv 0 1 0\n"
        "f 1 2 3\nf 1 3 4\n"
        "cstype bezier\ndeg 2\ncurv 0 1 1 2 3\nend\n"
        "cstype rat bezier\ndeg 2\ncurv 0 1 1 2 3\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "mixed legacy input parse3");
    ++g_source_cases;
    const auto &shapes = pp.shapes();
    size_t faces = 0;
    for (size_t i = 0; i < shapes.size(); ++i)
      faces += shapes[i].mesh.num_face_vertices.size();
    CHECK(faces == 2, "both triangles preserved");
    bool any_curve = false;
    for (size_t i = 0; i < shapes.size(); ++i)
      any_curve = any_curve || !shapes[i].curves.empty();
    CHECK(!any_curve, "bezier blocks ignored, no curves emitted");
    CHECK(pp.attrib().colors.size() == 12,
          "vertex colors still populated");
    ++g_source_cases;
  }

  // --- 7. Rational curve referencing plain xyz vertices uses weight 1.
  {
    const char *obj =
        "v 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "cstype rat bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "implicit-weight rational parse3");
    ++g_source_cases;
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    bool ok = tinyobj::TessellateCurve(pp.attrib(),
                                       pp.shapes()[0].curves[0], 1e-4,
                                       &lines, &err);
    CHECK(ok, "implicit unit weights tessellate");
    CHECK(lines.size() == 1 && lines[0].samples.size() >= 2,
          "polyline produced with implicit weights");
    ++g_source_cases;
  }
}

// ===========================================================================
// Group 2: rational and general curve geometry
// ===========================================================================

void test_rational_geometry() {
  const double tol = sizeof(tinyobj::real_t) < 8 ? 1e-4 : 1e-7;
  const double eps = sizeof(tinyobj::real_t) < 8 ? 2e-5 : 1e-9;

  // --- 1. Rational quarter circle: analytic arc, de Boor reference, exact
  // end points, and every chord within tolerance.
  {
    Paths pp = parse3(kQuarterCircleObj);
    CHECK(pp.all_ok(), "circle parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "circle tessellation");
    ++g_geom_cases;
    CHECK(lines.size() == 1, "circle is one continuous run");
    const auto &line = lines[0];

    // End points are exact and land on the requested parameter interval.
    CHECK(std::fabs(line.samples.front().param - 0.0) < eps &&
              std::fabs(line.samples.back().param - 1.0) < eps,
          "exact parameter endpoints");
    CHECK(std::fabs(line.samples.front().position[0] - 1.0) < eps &&
              std::fabs(line.samples.front().position[1] - 0.0) < eps,
          "exact start point");
    CHECK(std::fabs(line.samples.back().position[0] - 0.0) < eps &&
              std::fabs(line.samples.back().position[1] - 1.0) < eps,
          "exact end point");
    ++g_geom_cases;

    std::vector<RefPoint> cp;
    std::vector<double> weights;
    curve_reference_data(pp.attrib(), c, &cp, &weights);
    std::vector<double> knots(c.knots.begin(), c.knots.end());

    bool arc_ok = true;
    bool param_ok = true;
    double prev_u = -1.0;
    for (size_t i = 0; i < line.samples.size(); ++i) {
      double u = line.samples[i].param;
      if (!(u > prev_u)) param_ok = false;
      prev_u = u;

      // Independent Cox-de Boor evaluation at the stored parameter.
      RefPoint ref = ref_evaluate(cp, weights, knots, c.degree, u);
      RefPoint got = {line.samples[i].position[0],
                      line.samples[i].position[1],
                      line.samples[i].position[2]};
      if (distance3(ref, got) > eps) arc_ok = false;

      // Closed-form rational quadratic arc:
      //   D(u) = (1-u)^2 + 2w u(1-u) + u^2, w = sqrt(1/2)
      double w = std::sqrt(0.5);
      double D = (1 - u) * (1 - u) + 2 * w * u * (1 - u) + u * u;
      double ax = ((1 - u) * (1 - u) + 2 * w * u * (1 - u)) / D;
      double ay = (2 * w * u * (1 - u) + u * u) / D;
      if (std::fabs(got.x - ax) > eps || std::fabs(got.y - ay) > eps)
        arc_ok = false;
      if (std::fabs(got.x * got.x + got.y * got.y - 1.0) > eps * 10)
        arc_ok = false;  // true circular arc
    }
    CHECK(arc_ok, "samples match analytic arc and de Boor reference");
    CHECK(param_ok, "stored parameters strictly increasing");
    ++g_geom_cases;

    CHECK(verify_segments_within_tolerance(pp.attrib(), c, line, tol),
          "every circle chord within tolerance (dense reference check)");
    ++g_geom_cases;
  }

  // --- 2. Sub-interval of a multi-knot spline: only [lo, hi] is emitted,
  // with exact end points, checked against the de Boor reference.
  {
    const char *obj =
        "v 0 0 0\nv 1 2 0\nv 2 2 0\nv 3 0 0\n"
        "v 4 -2 0\nv 5 -2 0\nv 6 0 0\nv 7 0 0\n"
        "cstype bspline\ndeg 3\ncurv 0.25 1.75 1 2 3 4 5 6 7 8\n"
        "parm u 0 0 0 0 1 1 1 1 2 2 2 2\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "sub-interval parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "sub-interval tessellation");
    ++g_geom_cases;

    double lo = 1e9, hi = -1e9;
    for (size_t k = 0; k < lines.size(); ++k) {
      lo = std::min(lo, static_cast<double>(lines[k].samples.front().param));
      hi = std::max(hi, static_cast<double>(lines[k].samples.back().param));
    }
    CHECK(std::fabs(lo - 0.25) < eps && std::fabs(hi - 1.75) < eps,
          "output confined to curv sub-interval");
    ++g_geom_cases;

    bool ref_ok = true;
    for (size_t k = 0; k < lines.size(); ++k) {
      if (!verify_segments_within_tolerance(pp.attrib(), c, lines[k], tol))
        ref_ok = false;
      const auto &line = lines[k];
      std::vector<RefPoint> cp;
      std::vector<double> weights;
      curve_reference_data(pp.attrib(), c, &cp, &weights);
      std::vector<double> knots(c.knots.begin(), c.knots.end());
      RefPoint ref0 = ref_evaluate(cp, weights, knots, c.degree,
                                   line.samples.front().param);
      RefPoint got0 = {line.samples.front().position[0],
                       line.samples.front().position[1],
                       line.samples.front().position[2]};
      if (distance3(ref0, got0) > eps) ref_ok = false;
    }
    CHECK(ref_ok, "sub-interval geometry matches de Boor reference");
    ++g_geom_cases;
  }

  // --- 3. Cubic S-bend whose end points and midpoint all lie on the chord:
  // a midpoint-only flatness test would accept the straight chord, so the
  // tessellator must subdivide and reproduce the off-chord bulge.
  {
    const double h = 1.0;
    std::ostringstream ss;
    ss << "v 0 0 0\nv 1 " << h << " 0\nv 2 " << -h << " 0\nv 3 0 0\n"
       << "cstype bspline\ndeg 3\ncurv 0 1 1 2 3 4\n"
       << "parm u 0 0 0 0 1 1 1 1\nend\n";
    Paths pp = parse3(ss.str());
    CHECK(pp.all_ok(), "S-bend parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "S-bend tessellation");
    ++g_geom_cases;
    CHECK(lines.size() == 1, "S-bend is continuous");

    // Analytical cubic y(u) = 3h u(1-u)(1-2u): zero at u = 0, .5, 1 only.
    double max_abs_y = 0.0;
    for (size_t i = 0; i < lines[0].samples.size(); ++i)
      max_abs_y = std::max(max_abs_y,
                           static_cast<double>(
                               std::fabs(lines[0].samples[i].position[1])));
    CHECK(max_abs_y > tol,
          "S-bend is subdivided despite midpoint lying on the chord");
    ++g_geom_cases;

    std::vector<RefPoint> cp;
    std::vector<double> weights;
    curve_reference_data(pp.attrib(), c, &cp, &weights);
    std::vector<double> knots(c.knots.begin(), c.knots.end());
    RefPoint mid = ref_evaluate(cp, weights, knots, 3, 0.5);
    CHECK(std::fabs(mid.y) < eps, "reference confirms midpoint is on chord");
    CHECK(verify_segments_within_tolerance(pp.attrib(), c, lines[0], tol),
          "S-bend chords within tolerance (dense reference check)");
    ++g_geom_cases;
  }

  // --- 4. Degree-1 curve is exactly its control polygon.
  {
    const char *obj =
        "v 0 0 0\nv 2 0 0\nv 2 2 0\nv 0 2 0\n"
        "cstype bspline\ndeg 1\ncurv 0 3 1 2 3 4\n"
        "parm u 0 0 1 2 3 3\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "degree-1 parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "degree-1 tessellation");
    ++g_geom_cases;
    CHECK(lines.size() == 1 && lines[0].samples.size() == 4,
          "degree-1 emits one sample per knot span");
    bool exact = true;
    const double expect[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    for (int i = 0; i < 4; ++i)
      if (std::fabs(lines[0].samples[i].position[0] - expect[i][0]) > eps ||
          std::fabs(lines[0].samples[i].position[1] - expect[i][1]) > eps)
        exact = false;
    CHECK(exact, "degree-1 polyline equals the control polygon");
    ++g_geom_cases;
  }
}

// ===========================================================================
// Group 3: discontinuities
// ===========================================================================

void test_discontinuities() {
  const double tol = sizeof(tinyobj::real_t) < 8 ? 1e-4 : 1e-7;
  const double eps = sizeof(tinyobj::real_t) < 8 ? 2e-5 : 1e-9;

  // --- 1. Full break: interior knot with multiplicity degree+1 splits the
  // curve into two polylines that are never connected by a segment.
  {
    const char *obj =
        "v 0 0 0\nv 1 2 0\nv 2 2 0\nv 3 0 0\n"
        "v 4 -2 0\nv 5 -2 0\nv 6 0 0\nv 7 0 0\n"
        "cstype bspline\ndeg 3\ncurv 0 2 1 2 3 4 5 6 7 8\n"
        "parm u 0 0 0 0 1 1 1 1 2 2 2 2\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "full-break parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "full-break tessellation");
    ++g_disc_cases;
    CHECK(lines.size() == 2, "break yields exactly two polylines");
    if (lines.size() == 2) {
      CHECK(lines[0].samples.back().param == 1.0 &&
                lines[1].samples.front().param == 1.0,
            "both runs meet at the break parameter");
      CHECK(std::fabs(lines[0].samples.back().position[0] - 3.0) < eps &&
                std::fabs(lines[1].samples.front().position[0] - 4.0) < eps,
            "the two sides are distinct points (no connecting segment)");
      bool no_duplicate = true;
      for (size_t k = 0; k < 2; ++k)
        for (size_t i = 1; i < lines[k].samples.size(); ++i)
          if (!(lines[k].samples[i].param >
                lines[k].samples[i - 1].param))
            no_duplicate = false;
      CHECK(no_duplicate, "no duplicated parameters inside a run");

      std::vector<RefPoint> cp;
      std::vector<double> weights;
      curve_reference_data(pp.attrib(), c, &cp, &weights);
      std::vector<double> knots(c.knots.begin(), c.knots.end());
      bool ref_ok = true;
      for (int k = 0; k < 2; ++k) {
        if (!verify_segments_within_tolerance(pp.attrib(), c, lines[k], tol))
          ref_ok = false;
        // Each run's end points match the one-sided de Boor limits.
        RefPoint lhs = ref_evaluate_sided(
            cp, weights, knots, 3, lines[k].samples.front().param,
            lines[k].samples.front().param == 1.0 ? +1 : 0);
        RefPoint rhs = ref_evaluate_sided(
            cp, weights, knots, 3, lines[k].samples.back().param,
            lines[k].samples.back().param == 1.0 ? -1 : 0);
        RefPoint got0 = {lines[k].samples.front().position[0],
                         lines[k].samples.front().position[1],
                         lines[k].samples.front().position[2]};
        RefPoint got1 = {lines[k].samples.back().position[0],
                         lines[k].samples.back().position[1],
                         lines[k].samples.back().position[2]};
        if (distance3(lhs, got0) > eps || distance3(rhs, got1) > eps)
          ref_ok = false;
      }
      CHECK(ref_ok, "both sides match the de Boor one-sided limits");
    }
    ++g_disc_cases;
  }

  // --- 2. Continuous knot boundary (multiplicity < degree+1) stays one
  // polyline, with the boundary parameter kept exactly once.
  {
    const char *obj =
        "v 0 0 0\nv 1 2 0\nv 2 2 0\nv 3 0 0\nv 4 -2 0\nv 5 -2 0\n"
        "cstype bspline\ndeg 3\ncurv 0 2 1 2 3 4 5 6\n"
        "parm u 0 0 0 0 1 1 2 2 2 2\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "continuous-boundary parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "continuous-boundary tessellation");
    ++g_disc_cases;
    CHECK(lines.size() == 1, "continuous boundary does not split the line");
    int boundary_samples = 0;
    for (size_t i = 0; i < lines[0].samples.size(); ++i)
      if (lines[0].samples[i].param == 1.0) ++boundary_samples;
    CHECK(boundary_samples == 1, "knot boundary parameter kept once");
    CHECK(verify_segments_within_tolerance(pp.attrib(), c, lines[0], tol),
          "continuous-boundary chords within tolerance");
    ++g_disc_cases;
  }

  // --- 3. A break inside a requested sub-interval still splits the output.
  {
    const char *obj =
        "v 0 0 0\nv 1 2 0\nv 2 2 0\nv 3 0 0\n"
        "v 4 -2 0\nv 5 -2 0\nv 6 0 0\nv 7 0 0\n"
        "cstype bspline\ndeg 3\ncurv 0.5 1.5 1 2 3 4 5 6 7 8\n"
        "parm u 0 0 0 0 1 1 1 1 2 2 2 2\nend\n";
    Paths pp = parse3(obj);
    CHECK(pp.all_ok(), "sub-interval break parse3");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(TessellateCurve(pp.attrib(), c,
                          static_cast<tinyobj::real_t>(tol), &lines, &err),
          "sub-interval break tessellation");
    ++g_disc_cases;
    CHECK(lines.size() == 2, "break preserved inside sub-interval");
    if (lines.size() == 2) {
      CHECK(std::fabs(lines[0].samples.front().param - 0.5) < eps &&
                lines[0].samples.back().param == 1.0 &&
                lines[1].samples.front().param == 1.0 &&
                std::fabs(lines[1].samples.back().param - 1.5) < eps,
            "sub-interval runs cover exactly the requested parameter window");
    }
    ++g_disc_cases;
  }
}

// ===========================================================================
// Group 4: rejected input
// ===========================================================================

void test_invalid_blocks() {
  // 1. Unclosed block.
  {
    const char *obj =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\n";
    CHECK(reject3(obj), "unclosed block rejected on all paths");
    ++g_invalid_cases;
  }

  // 2. Missing required statements (deg / curv / parm).
  {
    const char *no_deg =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(no_deg), "missing deg rejected");
    const char *no_curv =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(no_curv), "missing curv rejected");
    const char *no_parm =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nend\n";
    CHECK(reject3(no_parm), "missing parm rejected");
    ++g_invalid_cases;
  }

  // 3. Knot count != control points + degree + 1.
  {
    const char *obj =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1\nend\n";
    CHECK(reject3(obj), "knot count mismatch rejected");
    ++g_invalid_cases;
  }

  // 4. Bad indices (out of range, including negative relative indices).
  {
    const char *too_big =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 9 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(too_big), "positive bad index rejected");
    const char *too_neg =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 -9 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(too_neg), "negative bad index rejected");
    ++g_invalid_cases;
  }

  // 5. Invalid parameter intervals.
  {
    const char *reversed =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 1 0 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(reversed), "reversed interval rejected");
    const char *outside =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 5 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(outside), "interval outside knots rejected");
    ++g_invalid_cases;
  }

  // 6. Non-finite numbers (knots and parameters).
  {
    const char *bad_knot =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 inf\nend\n";
    CHECK(reject3(bad_knot), "infinite knot rejected");
    const char *bad_param =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv nan 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(bad_param), "non-finite parameter rejected");
    ++g_invalid_cases;
  }

  // 7. Non-positive rational weights.
  {
    const char *zero_w =
        "v 1 0 0 1\nv 1 1 0 0\nv 0 1 0 1\n"
        "cstype rat bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(zero_w), "zero rational weight rejected");
    const char *neg_w =
        "v 1 0 0 1\nv 1 1 0 -2\nv 0 1 0 1\n"
        "cstype rat bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(neg_w), "negative rational weight rejected");
    // A literal infinite weight must never reach a curve as a non-finite
    // value. The default float parser normalizes `inf` to the max finite
    // sentinel, which curve finalization rejects; parsers that cannot read
    // the token instead drop it to a plain xyz vertex. Either way no
    // non-finite weight may survive into a tessellatable curve.
    const char *inf_w =
        "v 1 0 0 1\nv 1 1 0 inf\nv 0 1 0 1\n"
        "cstype rat bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    {
      Paths pp = parse3(inf_w);
      bool no_nonfinite = true;
      for (size_t i = 0; i < pp.attrib().vertex_weights.size(); ++i)
        if (!std::isfinite(pp.attrib().vertex_weights[i])) no_nonfinite = false;
      if (pp.all_ok()) {
        for (size_t i = 0; i < pp.shapes().size(); ++i)
          for (size_t k = 0; k < pp.shapes()[i].curves.size(); ++k) {
            std::vector<tinyobj::curve_polyline_t> lines;
            std::string err;
            bool ok =
                TessellateCurve(pp.attrib(), pp.shapes()[i].curves[k], 1e-3,
                                &lines, &err);
            no_nonfinite = no_nonfinite && ok && !lines.empty();
          }
      }
      CHECK(no_nonfinite, "infinite weight never produces non-finite curve");
    }
    ++g_invalid_cases;
  }

  // 8. Degree outside 1..3 and malformed knot ordering / non-clamped ends.
  {
    const char *bad_deg =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 4\ncurv 0 1 1 2 3\nparm u 0 0 0 1 1 1\nend\n";
    CHECK(reject3(bad_deg), "degree 4 rejected");
    const char *unclamped =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 1 1 1 1\nend\n";
    CHECK(reject3(unclamped), "non-clamped knot vector rejected");
    const char *decreasing =
        "v 0 0 0\nv 1 1 0\nv 2 0 0\n"
        "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 1 0 1 1\nend\n";
    CHECK(reject3(decreasing), "decreasing knots rejected");
    ++g_invalid_cases;
  }

  // 9. Tessellation API rejections: bad tolerance, and precision exhaustion
  // must fail rather than emit out-of-tolerance geometry.
  {
    Paths pp = parse3(kQuarterCircleObj);
    CHECK(pp.all_ok(), "circle parse3 for api checks");
    const tinyobj::curve_t &c = pp.shapes()[0].curves[0];
    std::vector<tinyobj::curve_polyline_t> lines;
    std::string err;
    CHECK(!TessellateCurve(pp.attrib(), c, 0.0, &lines, &err) &&
              !err.empty(),
          "zero tolerance rejected");
    lines.clear();
    err.clear();
    CHECK(!TessellateCurve(pp.attrib(), c, -1.0, &lines, &err) &&
              !err.empty(),
          "negative tolerance rejected");
    ++g_invalid_cases;

    lines.clear();
    err.clear();
    bool ok = TessellateCurve(pp.attrib(), c, 1e-300, &lines, &err);
    CHECK(!ok && lines.empty() && !err.empty(),
          "unreachable tolerance fails without out-of-tolerance output");
    ++g_invalid_cases;
  }
}

}  // namespace

int main() {
  std::srand(12345);
  test_source_state();
  test_rational_geometry();
  test_discontinuities();
  test_invalid_blocks();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d freeform regression check(s) failed\n",
                 g_failures);
    return 1;
  }
  std::printf("PASS source-state cases=%d\n", g_source_cases);
  std::printf("PASS rational-geometry cases=%d\n", g_geom_cases);
  std::printf("PASS discontinuities cases=%d\n", g_disc_cases);
  std::printf("PASS invalid-blocks cases=%d\n", g_invalid_cases);
  std::printf("TINYOBJ_FREEFORM_OK\n");
  return 0;
}
