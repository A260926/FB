#include "minipbrt_scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <queue>
#include <utility>
#include <vector>

namespace minipbrt {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

const double kAffineTolerance = 1e-9;
const size_t kMaxPolynomialDegree = 64;

inline bool finite_d(double v) { return std::isfinite(v); }

inline bool finite_ld(long double v) { return std::isfinite(v); }


struct WalkData {
  // Owner object for each shape, or kInvalidIndex for world-direct shapes.
  std::vector<uint32_t> shapeOwner;
  // Instances contained in each object, in `Scene::instances` order.
  std::vector<std::vector<uint32_t> > childInstances;
  // Instances directly in the world, in `Scene::instances` order.
  std::vector<uint32_t> rootInstances;
};


struct Target {
  uint32_t shape;
  std::vector<uint32_t> path;
};


static bool fail(std::string& error, const std::string& message)
{
  error = message;
  return false;
}


template <typename T>
static bool matrix_is_finite(const T m[4][4])
{
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!std::isfinite(static_cast<long double>(m[r][c]))) {
        return false;
      }
    }
  }
  return true;
}


template <typename T>
static long double matrix_max_abs(const T m[4][4])
{
  long double scale = 0.0L;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      long double a = std::fabs(static_cast<long double>(m[r][c]));
      if (a > scale) {
        scale = a;
      }
    }
  }
  return scale;
}


template <typename T>
static bool matrix_is_affine_impl(const T& m)
{
  long double scale = 0.0L;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      long double a = std::fabs(static_cast<long double>(m[r][c]));
      if (a > scale) {
        scale = a;
      }
    }
  }
  long double tolerance = kAffineTolerance * (1.0L + scale);
  return std::fabs(static_cast<long double>(m[3][0])) <= tolerance &&
         std::fabs(static_cast<long double>(m[3][1])) <= tolerance &&
         std::fabs(static_cast<long double>(m[3][2])) <= tolerance &&
         std::fabs(static_cast<long double>(m[3][3]) - 1.0L) <= tolerance;
}


static bool matrix_is_affine(const float m[4][4])
{
  return matrix_is_affine_impl(m);
}


static bool matrix_is_affine(const double m[4][4])
{
  return matrix_is_affine_impl(m);
}


static bool validate_transform(const Transform& transform, const char* what,
                               std::string& error)
{
  if (!matrix_is_finite(transform.start) || !matrix_is_finite(transform.end)) {
    return fail(error, std::string(what) + " contains non-finite matrix elements");
  }
  if (!matrix_is_affine(transform.start) || !matrix_is_affine(transform.end)) {
    return fail(error, std::string(what) + " is not an affine transform");
  }
  return true;
}


// Interpolate two endpoint matrices at the normalised scene time `u`.
static void interpolate_matrix(const Transform& transform, double u, double out[4][4])
{
  const double v = 1.0 - u;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      out[r][c] = v * static_cast<double>(transform.start[r][c]) +
                  u * static_cast<double>(transform.end[r][c]);
    }
  }
}


static void identity_matrix(double m[4][4])
{
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      m[r][c] = (r == c) ? 1.0 : 0.0;
    }
  }
}


static void multiply_matrix(const double a[4][4], const double b[4][4], double out[4][4])
{
  double result[4][4];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      long double sum = 0.0L;
      for (int k = 0; k < 4; k++) {
        sum += static_cast<long double>(a[r][k]) * static_cast<long double>(b[k][c]);
      }
      result[r][c] = static_cast<double>(sum);
    }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      out[r][c] = result[r][c];
    }
  }
}


// Determinant and inverse-transpose of the linear 3x3 part of an affine
// matrix. `normal` receives the row-major inverse transpose.
static bool invert_linear_part(const double world[4][4], double normal[3][3])
{
  long double a00 = world[0][0], a01 = world[0][1], a02 = world[0][2];
  long double a10 = world[1][0], a11 = world[1][1], a12 = world[1][2];
  long double a20 = world[2][0], a21 = world[2][1], a22 = world[2][2];

  long double c00 =  (a11 * a22 - a12 * a21);
  long double c01 = -(a10 * a22 - a12 * a20);
  long double c02 =  (a10 * a21 - a11 * a20);

  long double c10 = -(a01 * a22 - a02 * a21);
  long double c11 =  (a00 * a22 - a02 * a20);
  long double c12 = -(a00 * a21 - a01 * a20);

  long double c20 =  (a01 * a12 - a02 * a11);
  long double c21 = -(a00 * a12 - a02 * a10);
  long double c22 =  (a00 * a11 - a01 * a10);

  long double det = a00 * c00 + a01 * c01 + a02 * c02;

  long double row0 = std::sqrt(a00 * a00 + a01 * a01 + a02 * a02);
  long double row1 = std::sqrt(a10 * a10 + a11 * a11 + a12 * a12);
  long double row2 = std::sqrt(a20 * a20 + a21 * a21 + a22 * a22);
  long double scale = row0 * row1 * row2;

  if (!finite_ld(det) || !finite_ld(scale) || scale == 0.0L) {
    return false;
  }
  if (std::fabs(det) <= 1e-12L * scale) {
    return false; // Singular, or so close to singular that it's not usable.
  }

  long double invDet = 1.0L / det;
  normal[0][0] = static_cast<double>(c00 * invDet);
  normal[0][1] = static_cast<double>(c01 * invDet);
  normal[0][2] = static_cast<double>(c02 * invDet);
  normal[1][0] = static_cast<double>(c10 * invDet);
  normal[1][1] = static_cast<double>(c11 * invDet);
  normal[1][2] = static_cast<double>(c12 * invDet);
  normal[2][0] = static_cast<double>(c20 * invDet);
  normal[2][1] = static_cast<double>(c21 * invDet);
  normal[2][2] = static_cast<double>(c22 * invDet);

  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 3; c++) {
      if (!finite_d(normal[r][c])) {
        return false;
      }
    }
  }
  return true;
}


static double clamp_time(double time, double start, double end)
{
  if (time <= start) {
    return start;
  }
  if (time >= end) {
    return end;
  }
  return time;
}


static double normalised_time(const Scene& scene, double time)
{
  double dt = static_cast<double>(scene.endTime) - static_cast<double>(scene.startTime);
  if (dt == 0.0) {
    return 0.0;
  }
  double clamped = clamp_time(time, scene.startTime, scene.endTime);
  return (clamped - static_cast<double>(scene.startTime)) / dt;
}


// ---------------------------------------------------------------------------
// Structural validation and traversal
// ---------------------------------------------------------------------------

static bool validate_mesh(const TriangleMesh* mesh, uint32_t shapeIndex,
                          std::string& error)
{
  if (mesh->num_indices % 3 != 0) {
    return fail(error, "Triangle mesh shape " + std::to_string(shapeIndex) +
                       " has an index count that is not a multiple of 3");
  }
  if (mesh->num_indices > 0 && mesh->indices == nullptr) {
    return fail(error, "Triangle mesh shape " + std::to_string(shapeIndex) +
                       " has no index array");
  }
  if (mesh->num_vertices > 0 && mesh->P == nullptr) {
    return fail(error, "Triangle mesh shape " + std::to_string(shapeIndex) +
                       " has no vertex array");
  }
  for (unsigned int i = 0; i < mesh->num_indices; i++) {
    int index = mesh->indices[i];
    if (index < 0 || static_cast<unsigned int>(index) >= mesh->num_vertices) {
      return fail(error, "Triangle mesh shape " + std::to_string(shapeIndex) +
                         " has an out-of-range vertex index");
    }
  }
  for (unsigned int i = 0; i < mesh->num_vertices * 3u; i++) {
    if (!finite_d(mesh->P[i])) {
      return fail(error, "Triangle mesh shape " + std::to_string(shapeIndex) +
                         " has a non-finite vertex coordinate");
    }
  }
  return true;
}


static bool validate_scene(const Scene& scene, WalkData& walk, std::string& error)
{
  if (!finite_d(scene.startTime) || !finite_d(scene.endTime)) {
    return fail(error, "Scene time range is not finite");
  }
  if (scene.startTime > scene.endTime) {
    return fail(error, "Scene startTime is greater than endTime");
  }

  const uint32_t numShapes = static_cast<uint32_t>(scene.shapes.size());
  const uint32_t numObjects = static_cast<uint32_t>(scene.objects.size());

  // Validate object shape ranges and build the shape -> owner mapping.
  walk.shapeOwner.assign(numShapes, kInvalidIndex);
  for (uint32_t objectIndex = 0; objectIndex < numObjects; objectIndex++) {
    const Object* object = scene.objects[objectIndex];
    if (object == nullptr) {
      return fail(error, "Scene contains a null object");
    }

    const uint32_t first = object->firstShape;
    const uint32_t count = object->numShapes;
    if (count == 0) {
      if (first != kInvalidIndex && first > numShapes) {
        return fail(error, "Object " + std::to_string(objectIndex) +
                           " has an invalid shape range");
      }
      continue;
    }

    if (first == kInvalidIndex || first > numShapes ||
        count > numShapes - first) {
      return fail(error, "Object " + std::to_string(objectIndex) +
                         " has an invalid shape range");
    }

    for (uint32_t i = first; i < first + count; i++) {
      if (walk.shapeOwner[i] != kInvalidIndex) {
        return fail(error, "Object " + std::to_string(objectIndex) +
                           " has a shape range that overlaps another object");
      }
      walk.shapeOwner[i] = objectIndex;
    }
  }

  for (uint32_t i = 0; i < numShapes; i++) {
    const Shape* shape = scene.shapes[i];
    if (shape == nullptr) {
      return fail(error, "Scene contains a null shape");
    }
    if (shape->object != kInvalidIndex && shape->object >= numObjects) {
      return fail(error, "Shape " + std::to_string(i) + " refers to an invalid object");
    }
  }

  // Validate instances and group them by owner.
  walk.childInstances.assign(numObjects, std::vector<uint32_t>());
  walk.rootInstances.clear();
  for (uint32_t i = 0; i < static_cast<uint32_t>(scene.instances.size()); i++) {
    const Instance* instance = scene.instances[i];
    if (instance == nullptr) {
      return fail(error, "Scene contains a null instance");
    }
    if (instance->object == kInvalidIndex || instance->object >= numObjects) {
      return fail(error, "Instance " + std::to_string(i) + " refers to an invalid object");
    }
    if (instance->ownerObject != kInvalidIndex && instance->ownerObject >= numObjects) {
      return fail(error, "Instance " + std::to_string(i) + " has an invalid owner object");
    }
    if (instance->ownerObject == kInvalidIndex) {
      walk.rootInstances.push_back(i);
    }
    else {
      walk.childInstances[instance->ownerObject].push_back(i);
    }
  }

  // Detect cycles in the object reference graph.
  std::vector<uint32_t> colour(numObjects, 0); // 0 = unvisited, 1 = on path, 2 = done.
  struct Frame { uint32_t object; size_t child; };
  for (uint32_t start = 0; start < numObjects; start++) {
    if (colour[start] != 0) {
      continue;
    }
    std::vector<Frame> stack;
    stack.push_back(Frame{ start, 0 });
    colour[start] = 1;
    while (!stack.empty()) {
      Frame& frame = stack.back();
      if (frame.child < walk.childInstances[frame.object].size()) {
        uint32_t instanceIndex = walk.childInstances[frame.object][frame.child++];
        uint32_t child = scene.instances[instanceIndex]->object;
        if (colour[child] == 1) {
          return fail(error, "Object reference cycle detected in the scene");
        }
        if (colour[child] == 0) {
          colour[child] = 1;
          stack.push_back(Frame{ child, 0 });
        }
      }
      else {
        colour[frame.object] = 2;
        stack.pop_back();
      }
    }
  }

  return true;
}


// Recursively visit an object and add a target for every reachable shape.
static bool visit_object(const Scene& scene, const WalkData& walk,
                         uint32_t objectIndex, std::vector<uint32_t>& path,
                         std::vector<Target>& targets,
                         std::vector<bool>& onPath, std::string& error)
{
  if (objectIndex >= scene.objects.size() || onPath[objectIndex]) {
    return fail(error, "Object reference cycle detected in the scene");
  }
  onPath[objectIndex] = true;

  const Object* object = scene.objects[objectIndex];
  for (uint32_t i = 0; i < object->numShapes; i++) {
    Target target;
    target.shape = object->firstShape + i;
    target.path = path;
    targets.push_back(target);
  }

  for (uint32_t instanceIndex : walk.childInstances[objectIndex]) {
    path.push_back(instanceIndex);
    bool ok = visit_object(scene, walk, scene.instances[instanceIndex]->object,
                           path, targets, onPath, error);
    path.pop_back();
    if (!ok) {
      return false;
    }
  }

  onPath[objectIndex] = false;
  return true;
}


static bool build_targets(const Scene& scene, const WalkData& walk,
                          std::vector<Target>& targets, std::string& error)
{
  targets.clear();

  // World-direct shapes come first, in shape index order.
  for (uint32_t i = 0; i < static_cast<uint32_t>(scene.shapes.size()); i++) {
    if (walk.shapeOwner[i] == kInvalidIndex) {
      Target target;
      target.shape = i;
      targets.push_back(target);
    }
  }

  // Then each root instance in order.
  std::vector<bool> onPath(scene.objects.size(), false);
  std::vector<uint32_t> path;
  for (uint32_t instanceIndex : walk.rootInstances) {
    path.clear();
    path.push_back(instanceIndex);
    if (!visit_object(scene, walk, scene.instances[instanceIndex]->object,
                      path, targets, onPath, error)) {
      return false;
    }
  }

  return true;
}


static bool check_reachable_shapes(const Scene& scene, const WalkData& walk,
                                   const std::vector<Target>& targets,
                                   std::string& error)
{
  (void)walk;
  for (const Target& target : targets) {
    const Shape* shape = scene.shapes[target.shape];
    if (shape->type() != ShapeType::TriangleMesh) {
      return fail(error, "Reachable shape " + std::to_string(target.shape) +
                         " is not a triangle mesh");
    }
    const TriangleMesh* mesh = static_cast<const TriangleMesh*>(shape);
    if (!validate_mesh(mesh, target.shape, error)) {
      return false;
    }
    if (!validate_transform(shape->shapeToWorld, "Shape transformation", error)) {
      return false;
    }
    for (uint32_t instanceIndex : target.path) {
      if (!validate_transform(scene.instances[instanceIndex]->instanceToWorld,
                              "Instance transformation", error)) {
        return false;
      }
    }
  }
  return true;
}


// ---------------------------------------------------------------------------
// Polynomial machinery (Bernstein basis) for motion bounds
// ---------------------------------------------------------------------------

struct PolyMatrix {
  int degree;
  std::vector<long double> c; // (degree + 1) * 16 entries, row-major matrices.
};


static long double binomial(uint32_t n, uint32_t k)
{
  if (k > n) {
    return 0.0L;
  }
  if (k > n - k) {
    k = n - k;
  }
  long double result = 1.0L;
  for (uint32_t i = 0; i < k; i++) {
    result *= static_cast<long double>(n - i);
    result /= static_cast<long double>(i + 1);
  }
  return result;
}


static PolyMatrix poly_identity()
{
  PolyMatrix result;
  result.degree = 0;
  result.c.assign(16, 0.0L);
  for (int i = 0; i < 4; i++) {
    result.c[i * 4 + i] = 1.0L;
  }
  return result;
}


static PolyMatrix poly_multiply(const PolyMatrix& a, const PolyMatrix& b)
{
  PolyMatrix result;
  result.degree = a.degree + b.degree;
  result.c.assign(size_t(result.degree + 1) * 16, 0.0L);

  for (int i = 0; i <= a.degree; i++) {
    for (int j = 0; j <= b.degree; j++) {
      int k = i + j;
      long double weight = binomial(uint32_t(a.degree), uint32_t(i)) *
                           binomial(uint32_t(b.degree), uint32_t(j)) /
                           binomial(uint32_t(result.degree), uint32_t(k));
      const long double* am = &a.c[size_t(i) * 16];
      const long double* bm = &b.c[size_t(j) * 16];
      long double* cm = &result.c[size_t(k) * 16];
      for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
          long double sum = 0.0L;
          for (int t = 0; t < 4; t++) {
            sum += am[r * 4 + t] * bm[t * 4 + c];
          }
          cm[r * 4 + c] += weight * sum;
        }
      }
    }
  }
  return result;
}


static PolyMatrix poly_from_transform(const Transform& transform)
{
  PolyMatrix result;
  result.degree = 1;
  result.c.assign(2 * 16, 0.0L);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      result.c[r * 4 + c] = static_cast<long double>(transform.start[r][c]);
      result.c[16 + r * 4 + c] = static_cast<long double>(transform.end[r][c]);
    }
  }
  return result;
}


// Split a Bernstein polynomial at t, returning the control points for the
// two halves.
static void split_bernstein(const std::vector<long double>& b, long double t,
                            std::vector<long double>& left,
                            std::vector<long double>& right)
{
  const size_t n = b.size() - 1;
  left.assign(n + 1, 0.0L);
  right.assign(n + 1, 0.0L);
  if (n == 0) {
    left[0] = right[0] = b[0];
    return;
  }

  std::vector<long double> level = b;
  left[0] = b[0];
  right[n] = b[n];
  for (size_t r = 1; r <= n; r++) {
    for (size_t i = 0; i + 1 < level.size(); i++) {
      level[i] = (1.0L - t) * level[i] + t * level[i + 1];
    }
    level.pop_back();
    left[r] = level[0];
    right[n - r] = level[level.size() - 1];
  }
}


static long double eval_bernstein(const std::vector<long double>& b, long double t)
{
  if (b.empty()) {
    return 0.0L;
  }
  std::vector<long double> level = b;
  while (level.size() > 1) {
    for (size_t i = 0; i + 1 < level.size(); i++) {
      level[i] = (1.0L - t) * level[i] + t * level[i + 1];
    }
    level.pop_back();
  }
  return level[0];
}


// Restrict a Bernstein polynomial from [0,1] to [a,b] (with 0 <= a <= b <= 1).
static std::vector<long double> restrict_bernstein(const std::vector<long double>& b,
                                                   long double a, long double bParam)
{
  if (b.size() <= 1 || (a <= 0.0L && bParam >= 1.0L)) {
    return b;
  }
  if (a <= 0.0L) {
    std::vector<long double> left, right;
    split_bernstein(b, bParam, left, right);
    return left;
  }
  std::vector<long double> left, right;
  split_bernstein(b, a, left, right);
  if (bParam >= 1.0L) {
    return right;
  }
  long double local = (bParam - a) / (1.0L - a);
  std::vector<long double> left2, right2;
  split_bernstein(right, local, left2, right2);
  return left2;
}


struct Segment {
  std::vector<long double> c;
  long double lo;
  long double hi;
};


struct SegmentMaxOrder {
  bool operator()(const Segment& a, const Segment& b) const {
    return a.hi < b.hi; // priority_queue is a max-heap.
  }
};


static void compute_hull(const std::vector<long double>& c,
                         long double& lo, long double& hi)
{
  lo = hi = c[0];
  for (size_t i = 1; i < c.size(); i++) {
    if (c[i] < lo) lo = c[i];
    if (c[i] > hi) hi = c[i];
  }
  // The end control points are values of the polynomial on the interval.
  if (c.front() > hi) hi = c.front();
  if (c.back() > hi) hi = c.back();
  if (c.front() < lo) lo = c.front();
  if (c.back() < lo) lo = c.back();
}


// Find an upper bound for the maximum of a Bernstein polynomial on [0,1]
// which is no more than `tolerance` above the true maximum.
static long double upper_max(const std::vector<long double>& coefficients,
                             long double tolerance)
{
  if (coefficients.size() == 1) {
    return coefficients[0];
  }

  std::priority_queue<Segment, std::vector<Segment>, SegmentMaxOrder> queue;
  Segment initial;
  initial.c = coefficients;
  compute_hull(initial.c, initial.lo, initial.hi);
  long double bestLower = std::max(initial.c.front(), initial.c.back());
  queue.push(initial);

  // Invariant: bestLower is a value actually attained by the polynomial, so
  // it can never exceed the true maximum. When the largest remaining hull
  // upper bound is within `tolerance` of it, every interval is accounted for
  // and the true maximum lies in [bestLower, top.hi]. Adding half of the
  // tolerance covers floating point round-off in the hulls.
  const long double slack = 0.5L * tolerance;

  size_t guard = 0;
  while (!queue.empty()) {
    const Segment& top = queue.top();
    if (top.hi - bestLower <= slack || top.hi - top.lo <= slack) {
      // All remaining intervals lie below top.hi, and top.hi <= bestLower +
      // slack <= true maximum + slack <= true maximum + tolerance.
      return top.hi;
    }
    if (++guard > 4000000) {
      return top.hi;
    }

    Segment current = top;
    queue.pop();

    std::vector<long double> left, right;
    split_bernstein(current.c, 0.5L, left, right);
    Segment ls, rs;
    ls.c.swap(left);
    rs.c.swap(right);
    compute_hull(ls.c, ls.lo, ls.hi);
    compute_hull(rs.c, rs.lo, rs.hi);
    bestLower = std::max(bestLower, std::max(ls.c.back(), rs.c.back()));
    bestLower = std::max(bestLower, std::max(ls.c.front(), rs.c.front()));

    queue.push(ls);
    queue.push(rs);
  }
  return bestLower;
}


struct CoordinatePolynomials {
  std::vector<long double> x;
  std::vector<long double> y;
  std::vector<long double> z;
};


static CoordinatePolynomials vertex_polynomials(const PolyMatrix& world,
                                                const TriangleMesh* mesh,
                                                unsigned int vertex)
{
  long double px = static_cast<long double>(mesh->P[3 * vertex + 0]);
  long double py = static_cast<long double>(mesh->P[3 * vertex + 1]);
  long double pz = static_cast<long double>(mesh->P[3 * vertex + 2]);

  CoordinatePolynomials result;
  result.x.assign(size_t(world.degree + 1), 0.0L);
  result.y.assign(size_t(world.degree + 1), 0.0L);
  result.z.assign(size_t(world.degree + 1), 0.0L);

  for (int k = 0; k <= world.degree; k++) {
    const long double* m = &world.c[size_t(k) * 16];
    result.x[k] = m[0] * px + m[1] * py + m[2] * pz + m[3];
    result.y[k] = m[4] * px + m[5] * py + m[6] * pz + m[7];
    result.z[k] = m[8] * px + m[9] * py + m[10] * pz + m[11];
  }
  return result;
}


static bool build_world_polynomial(const Scene& scene, const Target& target,
                                   PolyMatrix& world, std::string& error)
{
  world = poly_identity();
  for (uint32_t instanceIndex : target.path) {
    if (world.degree >= int(kMaxPolynomialDegree)) {
      return fail(error, "Instance path is too deep to bound");
    }
    world = poly_multiply(world, poly_from_transform(
        scene.instances[instanceIndex]->instanceToWorld));
  }
  if (world.degree >= int(kMaxPolynomialDegree)) {
    return fail(error, "Instance path is too deep to bound");
  }
  world = poly_multiply(world, poly_from_transform(scene.shapes[target.shape]->shapeToWorld));
  return true;
}

} // namespace


// ---------------------------------------------------------------------------
// sample_scene
// ---------------------------------------------------------------------------

bool sample_scene(const Scene& scene, double time,
                  std::vector<SceneDraw>& out, std::string& error)
{
  if (!finite_d(time)) {
    return fail(error, "Sample time is not finite");
  }

  WalkData walk;
  if (!validate_scene(scene, walk, error)) {
    return false;
  }

  std::vector<Target> targets;
  if (!build_targets(scene, walk, targets, error)) {
    return false;
  }
  if (!check_reachable_shapes(scene, walk, targets, error)) {
    return false;
  }

  const double u = normalised_time(scene, time);

  std::vector<SceneDraw> results;
  results.reserve(targets.size());

  for (const Target& target : targets) {
    const Shape* shape = scene.shapes[target.shape];

    double world[4][4];
    identity_matrix(world);
    for (uint32_t instanceIndex : target.path) {
      double matrix[4][4];
      interpolate_matrix(scene.instances[instanceIndex]->instanceToWorld, u, matrix);
      multiply_matrix(world, matrix, world);
    }
    double shapeMatrix[4][4];
    interpolate_matrix(shape->shapeToWorld, u, shapeMatrix);
    multiply_matrix(world, shapeMatrix, world);

    if (!matrix_is_finite(world)) {
      return fail(error, "World matrix is not finite at the requested time");
    }
    if (!matrix_is_affine(world)) {
      return fail(error, "World matrix is not affine at the requested time");
    }

    SceneDraw draw;
    draw.shape = target.shape;
    draw.path = target.path;
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        draw.world[r][c] = world[r][c];
      }
    }
    if (!invert_linear_part(world, draw.normal)) {
      return fail(error, "World matrix is not invertible at the requested time");
    }

    double determinant =
        world[0][0] * (world[1][1] * world[2][2] - world[1][2] * world[2][1]) -
        world[0][1] * (world[1][0] * world[2][2] - world[1][2] * world[2][0]) +
        world[0][2] * (world[1][0] * world[2][1] - world[1][1] * world[2][0]);

    bool flip = shape->reverseOrientation;
    for (uint32_t instanceIndex : target.path) {
      flip = flip != scene.instances[instanceIndex]->reverseOrientation;
    }
    flip = flip != (determinant < 0.0);
    draw.flipOrientation = flip;

    results.push_back(draw);
  }

  out.swap(results);
  error.clear();
  return true;
}


// ---------------------------------------------------------------------------
// bound_scene_motion
// ---------------------------------------------------------------------------

bool bound_scene_motion(const Scene& scene, double begin, double end,
                        double epsilon,
                        std::vector<SceneMotionBounds>& out,
                        std::string& error)
{
  if (!finite_d(begin) || !finite_d(end)) {
    return fail(error, "Motion bound time range is not finite");
  }
  if (begin > end) {
    return fail(error, "Motion bound begin is greater than end");
  }
  if (!finite_d(epsilon) || epsilon <= 0.0) {
    return fail(error, "Motion bound epsilon must be finite and positive");
  }

  WalkData walk;
  if (!validate_scene(scene, walk, error)) {
    return false;
  }

  std::vector<Target> targets;
  if (!build_targets(scene, walk, targets, error)) {
    return false;
  }
  if (!check_reachable_shapes(scene, walk, targets, error)) {
    return false;
  }

  // Clamp the requested interval to the scene's time range.
  const double clampedBegin = clamp_time(begin, scene.startTime, scene.endTime);
  const double clampedEnd = clamp_time(end, scene.startTime, scene.endTime);
  const double u0 = normalised_time(scene, clampedBegin);
  const double u1 = normalised_time(scene, clampedEnd);

  std::vector<SceneMotionBounds> results;
  results.reserve(targets.size());

  for (const Target& target : targets) {
    const TriangleMesh* mesh = static_cast<const TriangleMesh*>(scene.shapes[target.shape]);

    PolyMatrix world;
    if (!build_world_polynomial(scene, target, world, error)) {
      return false;
    }

    // Compute a lower bound for the maximum absolute coordinate of the
    // trajectory. Using a lower bound (rather than an estimate) keeps the
    // resulting tolerance within the contract even when the true maximum is
    // attained in the interior of the interval.
    long double sLower = 0.0L;
    long double controlScale = 0.0L;
    if (u0 != u1) {
      for (unsigned int vertex = 0; vertex < mesh->num_vertices; vertex++) {
        CoordinatePolynomials poly = vertex_polynomials(world, mesh, vertex);
        std::vector<long double>* axes[3] = { &poly.x, &poly.y, &poly.z };
        for (int axis = 0; axis < 3; axis++) {
          std::vector<long double> restricted = restrict_bernstein(
              *axes[axis], static_cast<long double>(u0), static_cast<long double>(u1));
          const long double samples[5] = { 0.0L, 0.25L, 0.5L, 0.75L, 1.0L };
          for (int s = 0; s < 5; s++) {
            long double value = eval_bernstein(restricted, samples[s]);
            long double magnitude = std::fabs(value);
            if (magnitude > sLower) {
              sLower = magnitude;
            }
          }
          for (size_t i = 0; i < restricted.size(); i++) {
            long double magnitude = std::fabs(restricted[i]);
            if (magnitude > controlScale) {
              controlScale = magnitude;
            }
          }
        }
      }
    }
    else {
      // Instantaneous interval: just evaluate the matrix at the single time.
      for (unsigned int vertex = 0; vertex < mesh->num_vertices; vertex++) {
        CoordinatePolynomials poly = vertex_polynomials(world, mesh, vertex);
        std::vector<long double>* axes[3] = { &poly.x, &poly.y, &poly.z };
        for (int axis = 0; axis < 3; axis++) {
          long double value = eval_bernstein(*axes[axis], static_cast<long double>(u0));
          long double magnitude = std::fabs(value);
          if (magnitude > sLower) sLower = magnitude;
          if (magnitude > controlScale) controlScale = magnitude;
        }
      }
    }

    // Shave a tiny amount off sLower so rounding cannot make it exceed the
    // true maximum.
    sLower *= (1.0L - 1e-12L);
    long double sForTolerance = std::max(1.0L, sLower);
    long double tolerance = static_cast<long double>(epsilon) +
                            1e-8L * sForTolerance;

    // A small safety margin for floating point round-off in our bounds. It is
    // included in the tolerance budget rather than added on top of it.
    long double pad = 1e-13L * (1.0L + controlScale);
    if (pad > 0.5L * tolerance) {
      pad = 0.5L * tolerance;
    }
    long double targetTolerance = tolerance - pad;
    if (!(targetTolerance > 0.0L)) {
      targetTolerance = 0.25L * tolerance;
    }

    long double boxMin[3] = { 1e300L, 1e300L, 1e300L };
    long double boxMax[3] = { -1e300L, -1e300L, -1e300L };
    bool haveVertex = false;

    for (unsigned int vertex = 0; vertex < mesh->num_vertices; vertex++) {
      CoordinatePolynomials poly = vertex_polynomials(world, mesh, vertex);
      std::vector<long double>* axes[3] = { &poly.x, &poly.y, &poly.z };
      for (int axis = 0; axis < 3; axis++) {
        std::vector<long double> restricted = restrict_bernstein(
            *axes[axis], static_cast<long double>(u0), static_cast<long double>(u1));

        long double upper = upper_max(restricted, targetTolerance);
        std::vector<long double> negated(restricted.size());
        for (size_t i = 0; i < restricted.size(); i++) {
          negated[i] = -restricted[i];
        }
        long double negUpper = upper_max(negated, targetTolerance);
        long double lower = -negUpper;

        long double high = upper + pad;
        long double low = lower - pad;

        // Make the box include the endpoints explicitly.
        long double endpoint0 = restricted.front();
        long double endpoint1 = restricted.back();
        if (endpoint0 > high) high = endpoint0;
        if (endpoint1 > high) high = endpoint1;
        if (endpoint0 < low) low = endpoint0;
        if (endpoint1 < low) low = endpoint1;

        if (low < boxMin[axis]) boxMin[axis] = low;
        if (high > boxMax[axis]) boxMax[axis] = high;
        haveVertex = true;
      }
    }

    SceneMotionBounds bounds;
    bounds.shape = target.shape;
    bounds.path = target.path;
    if (haveVertex) {
      for (int axis = 0; axis < 3; axis++) {
        bounds.min[axis] = static_cast<double>(boxMin[axis]);
        bounds.max[axis] = static_cast<double>(boxMax[axis]);
      }
    }
    else {
      for (int axis = 0; axis < 3; axis++) {
        bounds.min[axis] = 0.0;
        bounds.max[axis] = 0.0;
      }
    }
    results.push_back(bounds);
  }

  out.swap(results);
  error.clear();
  return true;
}

} // namespace minipbrt
