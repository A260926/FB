#include "minipbrt_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace minipbrt {
namespace {

struct PathRecord {
  uint32_t shape = kInvalidIndex;
  std::vector<uint32_t> path;
};

struct Mat4d {
  double a[4][4];
};

static bool finite_float(float x) {
  return std::isfinite(static_cast<double>(x));
}

static bool valid_transform(const Transform& transform, std::string& error) {
  for (int endpoint = 0; endpoint < 2; ++endpoint) {
    const float (*m)[4] = (endpoint == 0) ? transform.start : transform.end;
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        if (!finite_float(m[i][j])) {
          error = "Scene contains a non-finite transform matrix";
          return false;
        }
      }
    }

    // Row-major affine matrix acting on column vectors:
    // last row must be [0 0 0 1].
    if (m[3][0] != 0.0f || m[3][1] != 0.0f || m[3][2] != 0.0f || m[3][3] != 1.0f) {
      error = "Scene contains a non-affine transform matrix";
      return false;
    }
  }
  return true;
}

static bool scene_time_valid(const Scene& scene, std::string& error) {
  double begin = static_cast<double>(scene.startTime);
  double end = static_cast<double>(scene.endTime);
  if (!std::isfinite(begin) || !std::isfinite(end) || begin > end) {
    error = "Scene time interval must be finite with startTime <= endTime";
    return false;
  }
  return true;
}

static bool graph_has_cycle(const Scene& scene, std::string& error) {
  const uint32_t objectCount = static_cast<uint32_t>(scene.objects.size());
  std::vector<std::vector<uint32_t> > children(objectCount);
  for (size_t i = 0; i < scene.instances.size(); ++i) {
    const Instance* instance = scene.instances[i];
    if (instance == nullptr || instance->ownerObject == kInvalidIndex) {
      continue;
    }
    if (instance->ownerObject < objectCount && instance->object < objectCount) {
      children[instance->ownerObject].push_back(instance->object);
    }
  }

  std::vector<unsigned char> color(objectCount, 0);
  struct Frame { uint32_t node; size_t next; };

  for (uint32_t start = 0; start < objectCount; ++start) {
    if (color[start] != 0) {
      continue;
    }
    std::vector<Frame> stack;
    color[start] = 1;
    stack.push_back(Frame{ start, 0 });

    while (!stack.empty()) {
      Frame& frame = stack.back();
      uint32_t node = frame.node;
      if (frame.next >= children[node].size()) {
        color[node] = 2;
        stack.pop_back();
        continue;
      }

      uint32_t target = children[node][frame.next++];
      if (color[target] == 0) {
        color[target] = 1;
        stack.push_back(Frame{ target, 0 });
      }
      else if (color[target] == 1) {
        error = "Scene object graph contains a reference cycle";
        return true;
      }
    }
  }
  return false;
}

// Validate all caller-constructed indices and arrays that could otherwise
// lead to out-of-range access.  Matrix validity is checked per reached path
// because Transform arrays have no default identity initializer.
static bool validate_topology(const Scene& scene, std::vector<unsigned char>& coveredShapes,
                              std::vector<std::vector<uint32_t> >& childInstances,
                              std::vector<uint32_t>& rootInstances,
                              std::string& error) {
  if (!scene_time_valid(scene, error)) {
    return false;
  }

  const size_t shapeCount = scene.shapes.size();
  const size_t objectCount = scene.objects.size();
  coveredShapes.assign(shapeCount, 0);
  childInstances.assign(objectCount, std::vector<uint32_t>());
  rootInstances.clear();

  for (size_t i = 0; i < shapeCount; ++i) {
    if (scene.shapes[i] == nullptr) {
      error = "Scene shapes array contains a null shape";
      return false;
    }
  }

  for (size_t i = 0; i < objectCount; ++i) {
    if (scene.objects[i] == nullptr) {
      error = "Scene objects array contains a null object";
      return false;
    }

    const Object& object = *scene.objects[i];
    if (object.numShapes > 0) {
      if (object.firstShape == kInvalidIndex) {
        error = "Object has numShapes but an invalid firstShape";
        return false;
      }
      uint64_t first = object.firstShape;
      uint64_t count = object.numShapes;
      if (first > shapeCount || count > shapeCount - first) {
        error = "Object firstShape/numShapes range is outside Scene::shapes";
        return false;
      }
      for (uint64_t s = first; s < first + count; ++s) {
        coveredShapes[static_cast<size_t>(s)] = 1;
        const Shape* shape = scene.shapes[static_cast<size_t>(s)];
        if (shape->object != kInvalidIndex && shape->object != i) {
          error = "Shape::object does not match its Object shape range";
          return false;
        }
      }
    }
  }

  for (size_t shapeIndex = 0; shapeIndex < shapeCount; ++shapeIndex) {
    const Shape* shape = scene.shapes[shapeIndex];
    if (shape->object != kInvalidIndex) {
      if (shape->object >= objectCount || scene.objects[shape->object] == nullptr) {
        error = "Shape::object index is invalid";
        return false;
      }
      const Object& owner = *scene.objects[shape->object];
      bool inRange = false;
      if (owner.numShapes > 0 && owner.firstShape != kInvalidIndex) {
        uint64_t first = owner.firstShape;
        uint64_t count = owner.numShapes;
        inRange = first <= shapeIndex && shapeIndex < first + count &&
                  first + count <= shapeCount;
      }
      if (!inRange) {
        error = "Shape::object is not consistent with the object's shape range";
        return false;
      }
    }

    if (shape->type() == ShapeType::TriangleMesh) {
      const TriangleMesh& mesh = static_cast<const TriangleMesh&>(*shape);
      if (mesh.num_vertices > 0 && mesh.P == nullptr) {
        error = "TriangleMesh has vertices but no P array";
        return false;
      }
      uint64_t floatCount = static_cast<uint64_t>(mesh.num_vertices) * 3u;
      for (uint64_t v = 0; v < floatCount; ++v) {
        if (!finite_float(mesh.P[static_cast<size_t>(v)])) {
          error = "TriangleMesh contains a non-finite vertex coordinate";
          return false;
        }
      }
      if (mesh.num_indices > 0) {
        if (mesh.indices == nullptr) {
          error = "TriangleMesh has indices but no indices array";
          return false;
        }
        if (mesh.num_indices % 3u != 0u) {
          error = "TriangleMesh index count is not a multiple of three";
          return false;
        }
        for (unsigned int k = 0; k < mesh.num_indices; ++k) {
          int index = mesh.indices[k];
          if (index < 0 || static_cast<unsigned int>(index) >= mesh.num_vertices) {
            error = "TriangleMesh index is outside its vertex array";
            return false;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < scene.instances.size(); ++i) {
    const Instance* instance = scene.instances[i];
    if (instance == nullptr) {
      error = "Scene instances array contains a null instance";
      return false;
    }
    if (instance->object == kInvalidIndex || instance->object >= objectCount ||
        scene.objects[instance->object] == nullptr) {
      error = "Instance has an invalid object index";
      return false;
    }
    if (instance->ownerObject != kInvalidIndex &&
        (instance->ownerObject >= objectCount || scene.objects[instance->ownerObject] == nullptr)) {
      error = "Instance has an invalid ownerObject index";
      return false;
    }

    if (instance->ownerObject == kInvalidIndex) {
      rootInstances.push_back(static_cast<uint32_t>(i));
    }
    else {
      childInstances[instance->ownerObject].push_back(static_cast<uint32_t>(i));
    }
  }

  if (graph_has_cycle(scene, error)) {
    return false;
  }
  return true;
}

static bool build_records(const Scene& scene, std::vector<PathRecord>& records,
                          std::string& error) {
  std::vector<unsigned char> covered;
  std::vector<std::vector<uint32_t> > children;
  std::vector<uint32_t> roots;
  if (!validate_topology(scene, covered, children, roots, error)) {
    return false;
  }

  records.clear();

  struct Builder {
    const Scene& scene;
    const std::vector<unsigned char>& covered;
    const std::vector<std::vector<uint32_t> >& children;
    std::vector<PathRecord>& records;
    std::string& error;

    bool add_shape(uint32_t shapeIndex, const std::vector<uint32_t>& path) {
      if (shapeIndex >= scene.shapes.size() || scene.shapes[shapeIndex] == nullptr) {
        error = "Path references an invalid shape";
        return false;
      }
      if (scene.shapes[shapeIndex]->type() != ShapeType::TriangleMesh) {
        error = "sample_scene only supports reachable TriangleMesh shapes";
        return false;
      }
      PathRecord record;
      record.shape = shapeIndex;
      record.path = path;
      records.push_back(record);
      return true;
    }

    bool visit_object(uint32_t objectIndex, std::vector<uint32_t>& path) {
      const Object& object = *scene.objects[objectIndex];
      if (object.numShapes > 0) {
        uint64_t first = object.firstShape;
        uint64_t count = object.numShapes;
        uint64_t end = first + count; // validate_topology already proved this is in range.
        for (uint64_t shapeIndex64 = first; shapeIndex64 < end; ++shapeIndex64) {
          uint32_t shapeIndex = static_cast<uint32_t>(shapeIndex64);
          if (!add_shape(shapeIndex, path)) {
            return false;
          }
        }
      }

      const std::vector<uint32_t>& instanceIndices = children[objectIndex];
      for (size_t k = 0; k < instanceIndices.size(); ++k) {
        uint32_t instanceIndex = instanceIndices[k];
        path.push_back(instanceIndex);
        uint32_t target = scene.instances[instanceIndex]->object;
        if (!visit_object(target, path)) {
          return false;
        }
        path.pop_back();
      }
      return true;
    }
  };

  Builder builder{ scene, covered, children, records, error };
  std::vector<uint32_t> path;

  // World-direct shapes first, in Scene::shapes order.  Covered shapes belong
  // to an Object range even if a directly constructed caller left Shape::object
  // at its default invalid value.
  for (uint32_t shapeIndex = 0; shapeIndex < scene.shapes.size(); ++shapeIndex) {
    if (!covered[shapeIndex]) {
      if (!builder.add_shape(shapeIndex, path)) {
        return false;
      }
    }
  }

  for (size_t r = 0; r < roots.size(); ++r) {
    uint32_t instanceIndex = roots[r];
    path.push_back(instanceIndex);
    uint32_t target = scene.instances[instanceIndex]->object;
    if (!builder.visit_object(target, path)) {
      return false;
    }
    path.pop_back();
  }

  return true;
}

static Mat4d identity_matrix() {
  Mat4d result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      result.a[i][j] = (i == j) ? 1.0 : 0.0;
    }
  }
  return result;
}

static Mat4d interpolate_transform(const Transform& transform, const Scene& scene,
                                   double time) {
  Mat4d result;
  double startTime = static_cast<double>(scene.startTime);
  double endTime = static_cast<double>(scene.endTime);
  double alpha = 0.0;
  if (startTime < endTime) {
    double clamped = std::min(endTime, std::max(startTime, time));
    alpha = (clamped - startTime) / (endTime - startTime);
  }

  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double a = static_cast<double>(transform.start[i][j]);
      double b = static_cast<double>(transform.end[i][j]);
      result.a[i][j] = a + alpha * (b - a);
    }
  }
  return result;
}

static Mat4d multiply_matrices(const Mat4d& a, const Mat4d& b) {
  Mat4d result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) {
        sum += a.a[i][k] * b.a[k][j];
      }
      result.a[i][j] = sum;
    }
  }
  return result;
}

static bool matrix_finite(const Mat4d& m) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      if (!std::isfinite(m.a[i][j])) {
        return false;
      }
    }
  }
  return true;
}

static bool path_world_matrix(const Scene& scene, const std::vector<uint32_t>& path,
                              double time, Mat4d& world, std::string& error) {
  if (!std::isfinite(time)) {
    error = "sample time must be finite";
    return false;
  }

  world = identity_matrix();
  for (size_t k = 0; k < path.size(); ++k) {
    uint32_t instanceIndex = path[k];
    if (instanceIndex >= scene.instances.size() || scene.instances[instanceIndex] == nullptr) {
      error = "Draw path contains an invalid instance index";
      return false;
    }
    const Instance& instance = *scene.instances[instanceIndex];
    if (!valid_transform(instance.instanceToWorld, error)) {
      return false;
    }
    Mat4d factor = interpolate_transform(instance.instanceToWorld, scene, time);
    world = multiply_matrices(world, factor);
    if (!matrix_finite(world)) {
      error = "World matrix became non-finite while multiplying instance transforms";
      return false;
    }
  }

  return true;
}

static double determinant3(const double m[3][3]) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
       - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
       + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

static bool invert_linear(const double m[3][3], double inverse[3][3], double& determinant) {
  determinant = determinant3(m);
  if (!std::isfinite(determinant) || determinant == 0.0) {
    return false;
  }

  inverse[0][0] =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / determinant;
  inverse[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]) / determinant;
  inverse[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / determinant;
  inverse[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]) / determinant;
  inverse[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / determinant;
  inverse[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]) / determinant;
  inverse[2][0] =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / determinant;
  inverse[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]) / determinant;
  inverse[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / determinant;

  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      if (!std::isfinite(inverse[i][j])) {
        return false;
      }
    }
  }
  return true;
}

} // anonymous namespace


bool sample_scene(const Scene& scene, double time,
                  std::vector<SceneDraw>& out, std::string& error) {
  if (!std::isfinite(time)) {
    error = "sample time must be finite";
    return false;
  }

  std::vector<PathRecord> records;
  if (!build_records(scene, records, error)) {
    return false;
  }

  std::vector<SceneDraw> result;
  result.reserve(records.size());

  for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
    const PathRecord& record = records[recordIndex];
    const Shape* shape = scene.shapes[record.shape];
    if (!valid_transform(shape->shapeToWorld, error)) {
      return false;
    }

    Mat4d world;
    if (!path_world_matrix(scene, record.path, time, world, error)) {
      return false;
    }
    Mat4d shapeMatrix = interpolate_transform(shape->shapeToWorld, scene, time);
    world = multiply_matrices(world, shapeMatrix);
    if (!matrix_finite(world)) {
      error = "World matrix is non-finite after multiplying shapeToWorld";
      return false;
    }

    double linear[3][3];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        linear[i][j] = world.a[i][j];
      }
    }

    double inverse[3][3];
    double determinant = 0.0;
    if (!invert_linear(linear, inverse, determinant)) {
      error = "World matrix is not invertible at the requested sample time";
      return false;
    }

    SceneDraw draw;
    draw.shape = record.shape;
    draw.path = record.path;
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        draw.world[i][j] = world.a[i][j];
      }
    }

    // normal = transpose(inverse(linear)).
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        draw.normal[i][j] = inverse[j][i];
      }
    }

    draw.flipOrientation = shape->reverseOrientation;
    for (size_t k = 0; k < record.path.size(); ++k) {
      draw.flipOrientation ^= scene.instances[record.path[k]]->reverseOrientation;
    }
    draw.flipOrientation ^= (determinant < 0.0);

    result.push_back(draw);
  }

  error.clear();
  out.swap(result);
  return true;
}

namespace {

// Long-double polynomial machinery for conservative motion extrema.
struct LPoly {
  std::vector<long double> c; // ascending monomial coefficients
};

static LPoly poly_constant(long double value) {
  LPoly p;
  p.c.push_back(value);
  return p;
}

static LPoly poly_linear(float startValue, float endValue) {
  long double a = static_cast<long double>(startValue);
  long double b = static_cast<long double>(endValue);
  LPoly p;
  p.c.push_back(a);
  p.c.push_back(b - a);
  while (p.c.size() > 1 && p.c.back() == 0.0L) {
    p.c.pop_back();
  }
  return p;
}

static LPoly poly_add(const LPoly& a, const LPoly& b) {
  LPoly result;
  result.c.assign(std::max(a.c.size(), b.c.size()), 0.0L);
  for (size_t i = 0; i < a.c.size(); ++i) result.c[i] += a.c[i];
  for (size_t i = 0; i < b.c.size(); ++i) result.c[i] += b.c[i];
  while (result.c.size() > 1 && result.c.back() == 0.0L) result.c.pop_back();
  return result;
}

static LPoly poly_multiply(const LPoly& a, const LPoly& b) {
  LPoly result;
  if (a.c.empty() || b.c.empty()) {
    return poly_constant(0.0L);
  }
  result.c.assign(a.c.size() + b.c.size() - 1, 0.0L);
  for (size_t i = 0; i < a.c.size(); ++i) {
    for (size_t j = 0; j < b.c.size(); ++j) {
      result.c[i + j] += a.c[i] * b.c[j];
    }
  }
  while (result.c.size() > 1 && result.c.back() == 0.0L) result.c.pop_back();
  return result;
}

static long double poly_eval(const LPoly& p, long double x) {
  if (p.c.empty()) {
    return 0.0L;
  }
  long double value = p.c.back();
  for (size_t i = p.c.size() - 1; i > 0; --i) {
    value = value * x + p.c[i - 1];
  }
  return value;
}

struct PolyMat4 {
  LPoly a[4][4];
};

static PolyMat4 polynomial_identity() {
  PolyMat4 m;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      m.a[i][j] = poly_constant((i == j) ? 1.0L : 0.0L);
    }
  }
  return m;
}

static PolyMat4 polynomial_factor(const Transform& transform) {
  PolyMat4 m;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      m.a[i][j] = poly_linear(transform.start[i][j], transform.end[i][j]);
    }
  }
  return m;
}

static PolyMat4 polynomial_multiply(const PolyMat4& a, const PolyMat4& b) {
  PolyMat4 result;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      LPoly sum = poly_constant(0.0L);
      for (int k = 0; k < 4; ++k) {
        sum = poly_add(sum, poly_multiply(a.a[i][k], b.a[k][j]));
      }
      result.a[i][j] = sum;
    }
  }
  return result;
}

static bool polynomial_matrix_finite(const PolyMat4& m) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      for (size_t k = 0; k < m.a[i][j].c.size(); ++k) {
        if (!std::isfinite(static_cast<double>(m.a[i][j].c[k]))) {
          return false;
        }
      }
    }
  }
  return true;
}

static LPoly row_coordinate_polynomial(const PolyMat4& world, int row,
                                       float x, float y, float z) {
  float p[4] = { x, y, z, 1.0f };
  LPoly result = poly_constant(0.0L);
  for (int j = 0; j < 4; ++j) {
    LPoly term = poly_multiply(world.a[row][j], poly_constant(static_cast<long double>(p[j])));
    result = poly_add(result, term);
  }
  return result;
}

struct IVal {
  long double lo;
  long double hi;
};

static long double negative_infinity_ld() {
  return -std::numeric_limits<long double>::infinity();
}

static long double positive_infinity_ld() {
  return std::numeric_limits<long double>::infinity();
}

static long double next_down_twice(long double x) {
  x = std::nextafter(x, negative_infinity_ld());
  return std::nextafter(x, negative_infinity_ld());
}

static long double next_up_twice(long double x) {
  x = std::nextafter(x, positive_infinity_ld());
  return std::nextafter(x, positive_infinity_ld());
}

static IVal interval_add(const IVal& a, const IVal& b) {
  return IVal{ next_down_twice(a.lo + b.lo), next_up_twice(a.hi + b.hi) };
}

static IVal interval_multiply(const IVal& a, const IVal& b) {
  long double lowProducts[4] = {
    next_down_twice(a.lo * b.lo),
    next_down_twice(a.lo * b.hi),
    next_down_twice(a.hi * b.lo),
    next_down_twice(a.hi * b.hi)
  };
  long double highProducts[4] = {
    next_up_twice(a.lo * b.lo),
    next_up_twice(a.lo * b.hi),
    next_up_twice(a.hi * b.lo),
    next_up_twice(a.hi * b.hi)
  };

  long double lo = lowProducts[0];
  long double hi = highProducts[0];
  for (int i = 1; i < 4; ++i) {
    lo = std::min(lo, lowProducts[i]);
    hi = std::max(hi, highProducts[i]);
  }
  return IVal{ lo, hi };
}

static IVal polynomial_interval(const LPoly& p, long double x0, long double x1) {
  IVal x{ x0, x1 };
  if (p.c.empty()) {
    return IVal{ 0.0L, 0.0L };
  }

  IVal acc{ p.c.back(), p.c.back() };
  for (size_t i = p.c.size() - 1; i > 0; --i) {
    IVal coefficient{ p.c[i - 1], p.c[i - 1] };
    acc = interval_add(interval_multiply(acc, x), coefficient);
  }
  return acc;
}

struct BoundContext {
  long double epsilon;
  long double observedScale;
};

static IVal bound_polynomial_recursive(const LPoly& p, long double left, long double right,
                                       int depth, BoundContext& context,
                                       long double& bestMinUpper,
                                       long double& bestMaxLower) {
  IVal interval = polynomial_interval(p, left, right);

  long double middle = (left + right) * 0.5L;
  if (middle < left || middle > right) {
    middle = left + (right - left) * 0.5L;
  }

  long double sampleX[3] = { left, middle, right };
  for (int i = 0; i < 3; ++i) {
    long double value = poly_eval(p, sampleX[i]);
    if (std::isfinite(static_cast<double>(value))) {
      context.observedScale = std::max(context.observedScale, std::fabs(value));
    }

    // A singleton interval gives outward-rounded upper/lower point bounds,
    // which keeps the stopping-gap argument conservative.
    IVal point = polynomial_interval(p, sampleX[i], sampleX[i]);
    bestMinUpper = std::min(bestMinUpper, point.hi);
    bestMaxLower = std::max(bestMaxLower, point.lo);
  }
  context.observedScale = std::max(context.observedScale, 1.0L);

  long double allowedGap = 0.25L * context.epsilon + 1e-9L * context.observedScale;
  bool lowerTight = (bestMinUpper - interval.lo) <= allowedGap;
  bool upperTight = (interval.hi - bestMaxLower) <= allowedGap;
  bool singleton = (middle == left || middle == right || left == right);

  if ((lowerTight && upperTight) || singleton || depth >= 96) {
    return interval;
  }

  IVal a = bound_polynomial_recursive(p, left, middle, depth + 1, context,
                                      bestMinUpper, bestMaxLower);
  IVal b = bound_polynomial_recursive(p, middle, right, depth + 1, context,
                                      bestMinUpper, bestMaxLower);
  return IVal{ std::min(a.lo, b.lo), std::max(a.hi, b.hi) };
}

static bool bound_polynomial(const LPoly& p, long double left, long double right,
                             BoundContext& context, long double& low, long double& high) {
  if (!std::isfinite(static_cast<double>(left)) ||
      !std::isfinite(static_cast<double>(right)) || left > right) {
    return false;
  }
  long double bestMinUpper = positive_infinity_ld();
  long double bestMaxLower = negative_infinity_ld();
  IVal result = bound_polynomial_recursive(p, left, right, 0, context,
                                           bestMinUpper, bestMaxLower);
  if (!std::isfinite(static_cast<double>(result.lo)) ||
      !std::isfinite(static_cast<double>(result.hi)) || result.lo > result.hi) {
    return false;
  }
  low = result.lo;
  high = result.hi;
  return true;
}

static bool time_parameter_interval(const Scene& scene, double begin, double end,
                                    long double& u0, long double& u1,
                                    std::string& error) {
  if (!std::isfinite(begin) || !std::isfinite(end) || begin > end) {
    error = "Motion bound interval must be finite with begin <= end";
    return false;
  }
  if (!scene_time_valid(scene, error)) {
    return false;
  }

  double startTime = static_cast<double>(scene.startTime);
  double endTime = static_cast<double>(scene.endTime);
  if (startTime == endTime) {
    u0 = u1 = 0.0L;
    return true;
  }

  double alphaBegin = (std::min(endTime, std::max(startTime, begin)) - startTime) /
                      (endTime - startTime);
  double alphaEnd = (std::min(endTime, std::max(startTime, end)) - startTime) /
                    (endTime - startTime);
  u0 = static_cast<long double>(alphaBegin);
  u1 = static_cast<long double>(alphaEnd);
  if (!std::isfinite(static_cast<double>(u0)) ||
      !std::isfinite(static_cast<double>(u1)) || u0 > u1) {
    error = "Motion bound time parameter is invalid";
    return false;
  }
  return true;
}

} // anonymous namespace


bool bound_scene_motion(const Scene& scene, double begin, double end, double epsilon,
                        std::vector<SceneMotionBounds>& out, std::string& error) {
  if (!std::isfinite(epsilon) || epsilon <= 0.0) {
    error = "Motion bound epsilon must be finite and positive";
    return false;
  }

  long double u0 = 0.0L;
  long double u1 = 0.0L;
  if (!time_parameter_interval(scene, begin, end, u0, u1, error)) {
    return false;
  }

  std::vector<PathRecord> records;
  if (!build_records(scene, records, error)) {
    return false;
  }

  std::vector<SceneMotionBounds> result;
  result.reserve(records.size());

  for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
    const PathRecord& record = records[recordIndex];
    const Shape* shape = scene.shapes[record.shape];
    if (!valid_transform(shape->shapeToWorld, error)) {
      return false;
    }

    PolyMat4 world = polynomial_identity();
    for (size_t k = 0; k < record.path.size(); ++k) {
      uint32_t instanceIndex = record.path[k];
      if (instanceIndex >= scene.instances.size() || scene.instances[instanceIndex] == nullptr) {
        error = "Motion path contains an invalid instance index";
        return false;
      }
      const Instance& instance = *scene.instances[instanceIndex];
      if (!valid_transform(instance.instanceToWorld, error)) {
        return false;
      }
      world = polynomial_multiply(world, polynomial_factor(instance.instanceToWorld));
      if (!polynomial_matrix_finite(world)) {
        error = "Motion matrix polynomial became non-finite";
        return false;
      }
    }
    world = polynomial_multiply(world, polynomial_factor(shape->shapeToWorld));
    if (!polynomial_matrix_finite(world)) {
      error = "Motion world matrix polynomial became non-finite";
      return false;
    }

    if (shape->type() != ShapeType::TriangleMesh) {
      error = "bound_scene_motion only supports reachable TriangleMesh shapes";
      return false;
    }
    const TriangleMesh& mesh = static_cast<const TriangleMesh&>(*shape);

    // S in the error contract is specific to this shape/path output.
    BoundContext context;
    context.epsilon = static_cast<long double>(epsilon);
    context.observedScale = 1.0L;

    long double rawMin[3] = {
      positive_infinity_ld(), positive_infinity_ld(), positive_infinity_ld()
    };
    long double rawMax[3] = {
      negative_infinity_ld(), negative_infinity_ld(), negative_infinity_ld()
    };

    if (mesh.num_vertices == 0) {
      for (int row = 0; row < 3; ++row) {
        rawMin[row] = 0.0L;
        rawMax[row] = 0.0L;
      }
    }

    for (uint64_t vertex = 0; vertex < static_cast<uint64_t>(mesh.num_vertices); ++vertex) {
      size_t floatBase = static_cast<size_t>(vertex * 3u);
      float x = mesh.P[floatBase + 0u];
      float y = mesh.P[floatBase + 1u];
      float z = mesh.P[floatBase + 2u];

      for (int row = 0; row < 3; ++row) {
        LPoly coordinate = row_coordinate_polynomial(world, row, x, y, z);
        long double low = 0.0L;
        long double high = 0.0L;
        if (!bound_polynomial(coordinate, u0, u1, context, low, high)) {
          error = "Failed to isolate a finite motion extrema";
          return false;
        }
        rawMin[row] = std::min(rawMin[row], low);
        rawMax[row] = std::max(rawMax[row], high);
      }
    }

    long double safety = 5e-9L * context.observedScale + 1e-12L;
    SceneMotionBounds bounds;
    bounds.shape = record.shape;
    bounds.path = record.path;
    for (int row = 0; row < 3; ++row) {
      long double conservativeMin = rawMin[row] - safety;
      long double conservativeMax = rawMax[row] + safety;
      if (!std::isfinite(static_cast<double>(conservativeMin)) ||
          !std::isfinite(static_cast<double>(conservativeMax))) {
        error = "Motion bound is non-finite";
        return false;
      }
      bounds.min[row] = static_cast<double>(conservativeMin);
      bounds.max[row] = static_cast<double>(conservativeMax);
    }
    result.push_back(bounds);
  }

  error.clear();
  out.swap(result);
  return true;
}

} // namespace minipbrt
