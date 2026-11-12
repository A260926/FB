/*
 * octasphere_geometry.cpp - headless geometric regression harness for the
 * par_octasphere generation chain.
 *
 * It does NOT modify the production algorithm and does not replace the
 * generator: every mesh comes from par_octasphere_populate() inside whatever
 * par_octasphere.h is on the include path. The harness independently rebuilds
 * the expected geometry (rounded-box signed-distance surface, double
 * precision sin/cos slerp sphere rows, welded manifold topology) and compares
 * it against the generated vertices, normals and indices.
 *
 * Usage:
 *   octasphere_geometry <output_dir> <mode>
 *     mode = clean   : every assertion family must pass.
 *     mode = angle   : the geodesic-sampling family must fail.
 *     mode = offset  : the surface-normal family must fail.
 *     mode = winding : the closed-topology family must fail.
 *
 * The mutation modes still generate every scenario first; a crash, timeout or
 * compile failure can never be reported as a detection because the process
 * would not reach the reporting stage and would exit non-zero.
 *
 * The reference math deliberately avoids paro_write_geodesic() and the
 * library's quaternion helpers - neither is called nor copied here.
 */
#define PAR_OCTASPHERE_IMPLEMENTATION
#include "par_octasphere.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

const double PI = 3.14159265358979323846;
const double EPS_DIM = 1e-5;       // dimensionless tolerance (directions)
const double EPS_REL = 1e-5;       // length tolerance as a fraction of scale
const float SENTINEL_F = -987654.321f;
const uint16_t SENTINEL_U16 = 0xFFFF;

struct Vec3 {
    double x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double clampd(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

/* Independent double-precision spherical interpolation along the great circle
 * through two unit vectors. Built only from sin/cos/acos - no quaternions. */
Vec3 slerp_unit(Vec3 a, Vec3 b, double t) {
    double cos_omega = clampd(dot(a, b), -1.0, 1.0);
    double omega = std::acos(cos_omega);
    if (omega < 1e-12) {
        return a;
    }
    double s = std::sin(omega);
    double k0 = std::sin((1.0 - t) * omega) / s;
    double k1 = std::sin(t * omega) / s;
    return a * k0 + b * k1;
}

struct ShapeConfig {
    const char* name;
    float radius;
    float width, height, depth;
    int subdivisions;
    bool sphere;
};

struct Mesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<uint16_t> indices;
    uint32_t capacity_indices = 0;
    uint32_t capacity_vertices = 0;
    uint32_t num_indices = 0;
    uint32_t num_vertices = 0;
    bool has_attributes = false;
};

struct Failure {
    std::string family;
    std::string detail;
};

struct ConfigStats {
    std::string name;
    int subdivisions = 0;
    uint32_t capacity_vertices = 0, capacity_indices = 0;
    uint32_t real_vertices = 0, real_indices = 0;
    int welded_vertices = 0, edges = 0, nondegenerate_faces = 0, degenerate = 0;
    int euler = 0, bad_edges = 0;
    double volume = 0, worst_distance_error = 0, worst_normal_error = 0;
    double worst_slerp_error = 0;
};

struct Report {
    std::vector<Failure> failures;
    std::vector<ConfigStats> stats;
    double worst_distance_error = 0;
    double worst_normal_error = 0;
    double worst_slerp_error = 0;
    double min_volume = 1e300;
    long total_triangles = 0, total_degenerate = 0, total_bad_edges = 0;

    void fail(const char* family, const char* fmt, ...) {
        char buffer[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
        failures.push_back({family, buffer});
    }
    bool family_failed(const std::string& family) const {
        for (const Failure& f : failures) {
            if (f.family == family) {
                return true;
            }
        }
        return false;
    }
};

double shape_scale(const ShapeConfig& c) {
    return std::max({(double)c.width, (double)c.height, (double)c.depth,
                     2.0 * (double)c.radius});
}

/* Allocate at the capacity upper bound, surround the usable range with
 * sentinels, run the generator, and verify the real write counts and ranges. */
Mesh generate_mesh(const ShapeConfig& c, bool with_attributes, Report& rep) {
    par_octasphere_config cfg = {};
    cfg.corner_radius = c.radius;
    cfg.width = c.width;
    cfg.height = c.height;
    cfg.depth = c.depth;
    cfg.num_subdivisions = c.subdivisions;
    cfg.uv_mode = PAR_OCTASPHERE_UV_LATLONG;
    cfg.normals_mode = PAR_OCTASPHERE_NORMALS_SMOOTH;

    uint32_t cap_indices = 0, cap_vertices = 0;
    par_octasphere_get_counts(&cfg, &cap_indices, &cap_vertices);

    Mesh mesh;
    mesh.capacity_indices = cap_indices;
    mesh.capacity_vertices = cap_vertices;
    mesh.has_attributes = with_attributes;

    // One extra sentinel slot on each side of every array.
    mesh.positions.assign(3 * (cap_vertices + 2), SENTINEL_F);
    mesh.indices.assign(cap_indices + 2, SENTINEL_U16);
    if (with_attributes) {
        mesh.normals.assign(3 * (cap_vertices + 2), SENTINEL_F);
        mesh.texcoords.assign(2 * (cap_vertices + 2), SENTINEL_F);
    }

    par_octasphere_mesh raw = {};
    raw.positions = mesh.positions.data() + 3;
    raw.indices = mesh.indices.data() + 1;
    raw.normals = with_attributes ? mesh.normals.data() + 3 : nullptr;
    raw.texcoords = with_attributes ? mesh.texcoords.data() + 2 : nullptr;

    par_octasphere_populate(&cfg, &raw);
    mesh.num_indices = raw.num_indices;
    mesh.num_vertices = raw.num_vertices;

    const char* book = "bookkeeping";
    if (mesh.num_vertices != cap_vertices) {
        rep.fail(book, "%s sub=%d: wrote %u vertices, capacity is %u", c.name,
                 c.subdivisions, mesh.num_vertices, cap_vertices);
    }
    if (mesh.num_indices == 0 || mesh.num_indices > cap_indices) {
        rep.fail(book, "%s sub=%d: wrote %u indices, capacity is %u", c.name,
                 c.subdivisions, mesh.num_indices, cap_indices);
    }
    if (mesh.num_indices % 3 != 0) {
        rep.fail(book, "%s sub=%d: index count %u is not a multiple of 3", c.name,
                 c.subdivisions, mesh.num_indices);
    }
    for (uint32_t i = 0; i < mesh.num_indices; ++i) {
        if (raw.indices[i] >= mesh.num_vertices) {
            rep.fail(book, "%s sub=%d: index %u out of range (%u vertices)", c.name,
                     c.subdivisions, raw.indices[i], mesh.num_vertices);
            break;
        }
    }
    // Sentinels before and after the capacity window must be untouched.
    if (mesh.positions[0] != SENTINEL_F || mesh.positions[1] != SENTINEL_F ||
        mesh.positions[2] != SENTINEL_F ||
        mesh.positions[3 * (cap_vertices + 1)] != SENTINEL_F ||
        mesh.positions[3 * (cap_vertices + 1) + 1] != SENTINEL_F ||
        mesh.positions[3 * (cap_vertices + 1) + 2] != SENTINEL_F) {
        rep.fail(book, "%s sub=%d: position array sentinel overwritten", c.name,
                 c.subdivisions);
    }
    if (mesh.indices[0] != SENTINEL_U16 || mesh.indices[cap_indices + 1] != SENTINEL_U16) {
        rep.fail(book, "%s sub=%d: index array sentinel overwritten", c.name,
                 c.subdivisions);
    }
    if (with_attributes) {
        if (mesh.normals[0] != SENTINEL_F ||
            mesh.normals[3 * (cap_vertices + 1)] != SENTINEL_F ||
            mesh.texcoords[0] != SENTINEL_F ||
            mesh.texcoords[2 * (cap_vertices + 1)] != SENTINEL_F) {
            rep.fail(book, "%s sub=%d: attribute array sentinel overwritten", c.name,
                     c.subdivisions);
        }
    }
    return mesh;
}

const float* position_of(const Mesh& m, uint32_t v) {
    return m.positions.data() + 3 + 3 * v;
}
const float* normal_of(const Mesh& m, uint32_t v) {
    return m.normals.data() + 3 + 3 * v;
}
uint16_t index_of(const Mesh& m, uint32_t i) {
    return m.indices[1 + i];
}

/* Every vertex must sit at exactly one radius from the inner axis-aligned
 * box, and the smooth normal must equal that outward direction - never the
 * normalized position relative to the origin. */
void check_surface_and_normals(const ShapeConfig& c, const Mesh& m, Report& rep,
                               ConfigStats& st) {
    const double r = c.radius;
    const double w = std::max((double)c.width, 2.0 * r);
    const double h = std::max((double)c.height, 2.0 * r);
    const double d = std::max((double)c.depth, 2.0 * r);
    const double half_x = w * 0.5 - r;
    const double half_y = h * 0.5 - r;
    const double half_z = d * 0.5 - r;
    const double scale = std::max({w, h, d});
    const double eps_len = EPS_REL * scale;

    for (uint32_t v = 0; v < m.num_vertices; ++v) {
        const float* p = position_of(m, v);
        double px = p[0], py = p[1], pz = p[2];
        double qx = clampd(px, -half_x, half_x);
        double qy = clampd(py, -half_y, half_y);
        double qz = clampd(pz, -half_z, half_z);
        Vec3 outward = {px - qx, py - qy, pz - qz};
        double dist = length(outward);
        double dist_err = std::fabs(dist - r);
        st.worst_distance_error = std::max(st.worst_distance_error, dist_err);
        rep.worst_distance_error = std::max(rep.worst_distance_error, dist_err);
        if (!(dist_err <= eps_len)) {
            rep.fail("surface-normal",
                     "%s sub=%d vertex %u: distance to inner box %.9g != r %.9g "
                     "(err %.3g, tol %.3g)",
                     c.name, c.subdivisions, v, dist, r, dist_err, eps_len);
            break;
        }
        if (m.has_attributes) {
            Vec3 expected = outward * (1.0 / dist);
            const float* n = normal_of(m, v);
            Vec3 actual = {n[0], n[1], n[2]};
            double nlen = length(actual);
            if (std::fabs(nlen - 1.0) > EPS_DIM) {
                rep.fail("surface-normal",
                         "%s sub=%d vertex %u: normal not unit length (%.9g)",
                         c.name, c.subdivisions, v, nlen);
                break;
            }
            double nerr = std::fabs(actual.x - expected.x) +
                          std::fabs(actual.y - expected.y) +
                          std::fabs(actual.z - expected.z);
            st.worst_normal_error = std::max(st.worst_normal_error, nerr);
            rep.worst_normal_error = std::max(rep.worst_normal_error, nerr);
            if (!(nerr <= EPS_DIM)) {
                rep.fail("surface-normal",
                         "%s sub=%d vertex %u: smooth normal (%.6f %.6f %.6f) "
                         "differs from outward radius (%.6f %.6f %.6f), L1 %.3g",
                         c.name, c.subdivisions, v, actual.x, actual.y, actual.z,
                         expected.x, expected.y, expected.z, nerr);
                break;
            }
        }
    }

    if (!m.has_attributes) {
        return;
    }

    // Smooth normals and outward face winding must agree. The spherical
    // patches are unfolded for every shape, so the sign check is valid there.
    int n = (1 << static_cast<int>(clampd(c.subdivisions, 0,
                                          PAR_OCTASPHERE_MAX_SUBDIVISIONS))) + 1;
    int triangles_per_patch = (n - 2) * (n - 1) + (n - 1);
    uint32_t patch_triangles = (uint32_t)triangles_per_patch * 8;
    double area_eps = 1e-12 * scale * scale;
    bool full_box = half_x > 0 && half_y > 0 && half_z > 0;

    uint32_t total_triangles = m.num_indices / 3;
    for (uint32_t tri = 0; tri < total_triangles; ++tri) {
        bool patch_region = tri < patch_triangles;
        if (!patch_region && !full_box) {
            break;  // folded capsule connector strips are checked topologically
        }
        uint16_t ia = index_of(m, tri * 3);
        uint16_t ib = index_of(m, tri * 3 + 1);
        uint16_t ic = index_of(m, tri * 3 + 2);
        const float* pa = position_of(m, ia);
        const float* pb = position_of(m, ib);
        const float* pc = position_of(m, ic);
        Vec3 a = {pa[0], pa[1], pa[2]};
        Vec3 b = {pb[0], pb[1], pb[2]};
        Vec3 cc = {pc[0], pc[1], pc[2]};
        Vec3 face = cross(b - a, cc - a);
        if (length(face) * 0.5 < area_eps) {
            continue;  // legal zero-area connector triangle
        }
        const float* na = normal_of(m, ia);
        const float* nb = normal_of(m, ib);
        const float* nc = normal_of(m, ic);
        Vec3 avg = {(na[0] + nb[0] + nc[0]), (na[1] + nb[1] + nc[1]),
                    (na[2] + nb[2] + nc[2])};
        if (dot(face, avg) <= 0) {
            rep.fail("surface-normal",
                     "%s sub=%d triangle %u: inward winding, face-normal dot %.6g",
                     c.name, c.subdivisions, tri, dot(face, avg) / length(face));
            break;
        }
    }
}

/* Rebuild the first octant's rows independently with double precision slerp
 * weights and compare point-by-point against the generated vertices. This
 * catches samples that stay on the sphere but slide to the wrong position. */
void check_geodesic_sampling(const ShapeConfig& c, const Mesh& m, Report& rep,
                             ConfigStats& st) {
    if (!c.sphere) {
        return;
    }
    int subdivisions = std::max(0, std::min(c.subdivisions, PAR_OCTASPHERE_MAX_SUBDIVISIONS));
    int n = (1 << subdivisions) + 1;
    const double r = c.radius;
    const double eps_len = EPS_REL * 2.0 * r;

    uint32_t vertex_index = 0;  // first octant begins at vertex 0
    for (int row = 0; row < n; ++row) {
        double theta = 0.5 * PI * row / (n - 1);
        Vec3 point_a = {0.0, std::sin(theta), std::cos(theta)};
        Vec3 point_b = {std::cos(theta), std::sin(theta), 0.0};
        int segments = n - 1 - row;
        for (int j = 0; j <= segments; ++j) {
            Vec3 reference;
            if (segments == 0) {
                // Level-0 pole row: a single boundary vertex, no interior.
                reference = point_a;
            } else {
                reference = slerp_unit(point_a, point_b, (double)j / segments);
            }
            reference = reference * r;
            const float* actual = position_of(m, vertex_index);
            double err = std::fabs(actual[0] - reference.x) +
                         std::fabs(actual[1] - reference.y) +
                         std::fabs(actual[2] - reference.z);
            st.worst_slerp_error = std::max(st.worst_slerp_error, err);
            rep.worst_slerp_error = std::max(rep.worst_slerp_error, err);
            if (!(err <= eps_len)) {
                rep.fail("geodesic-sampling",
                         "%s sub=%d row %d point %d: generated (%.9g %.9g %.9g) "
                         "but slerp reference is (%.9g %.9g %.9g), L1 %.3g tol %.3g",
                         c.name, c.subdivisions, row, j, actual[0], actual[1],
                         actual[2], reference.x, reference.y, reference.z, err,
                         eps_len);
                return;
            }
            ++vertex_index;
        }
    }
}

/* Weld only for inspection (never used to remove triangles), then require a
 * single closed oriented shell: every geometric edge shared by exactly two
 * oppositely oriented faces, Euler characteristic 2, positive signed volume. */
void check_closed_topology(const ShapeConfig& c, const Mesh& m, Report& rep,
                           ConfigStats& st) {
    const double scale = shape_scale(c);
    const double weld_tol = EPS_REL * scale;
    const double weld_tol_sq = weld_tol * weld_tol;

    std::vector<int> representative(m.num_vertices, -1);
    std::vector<uint32_t> canonical;
    for (uint32_t v = 0; v < m.num_vertices; ++v) {
        const float* p = position_of(m, v);
        int found = -1;
        for (uint32_t q : canonical) {
            const float* pq = position_of(m, q);
            double dx = p[0] - pq[0], dy = p[1] - pq[1], dz = p[2] - pq[2];
            if (dx * dx + dy * dy + dz * dz <= weld_tol_sq) {
                found = q;
                break;
            }
        }
        if (found < 0) {
            representative[v] = (int)canonical.size();
            canonical.push_back(v);
        } else {
            representative[v] = representative[found];
        }
    }

    struct EdgeInfo {
        int occurrences = 0;
        int first_from = -1, first_to = -1;
        bool opposite = false;
        bool duplicated_direction = false;
    };
    std::map<std::pair<int, int>, EdgeInfo> edges;
    int degenerate = 0;
    double volume = 0;
    uint32_t triangles = m.num_indices / 3;
    for (uint32_t tri = 0; tri < triangles; ++tri) {
        int a = representative[index_of(m, tri * 3)];
        int b = representative[index_of(m, tri * 3 + 1)];
        int cc = representative[index_of(m, tri * 3 + 2)];
        if (a == b || b == cc || a == cc) {
            ++degenerate;  // counted, kept, never silently deleted
            continue;
        }
        int loop[3][2] = {{a, b}, {b, cc}, {cc, a}};
        for (int k = 0; k < 3; ++k) {
            int u = loop[k][0], v = loop[k][1];
            auto key = std::make_pair(std::min(u, v), std::max(u, v));
            EdgeInfo& info = edges[key];
            if (info.occurrences == 0) {
                info.first_from = u;
                info.first_to = v;
            } else if (info.occurrences == 1) {
                info.opposite = (u == info.first_to && v == info.first_from);
            }
            if (info.occurrences >= 1 && u == info.first_from && v == info.first_to) {
                info.duplicated_direction = true;
            }
            ++info.occurrences;
        }
        const float* pa = position_of(m, index_of(m, tri * 3));
        const float* pb = position_of(m, index_of(m, tri * 3 + 1));
        const float* pc = position_of(m, index_of(m, tri * 3 + 2));
        volume += (pa[0] * (pb[1] * pc[2] - pb[2] * pc[1]) +
                   pa[1] * (pb[2] * pc[0] - pb[0] * pc[2]) +
                   pa[2] * (pb[0] * pc[1] - pb[1] * pc[0])) /
                  6.0;
    }

    int bad_edges = 0;
    for (const auto& entry : edges) {
        const EdgeInfo& info = entry.second;
        if (info.occurrences != 2 || !info.opposite || info.duplicated_direction) {
            ++bad_edges;
        }
    }
    int welded_v = (int)canonical.size();
    int edge_count = (int)edges.size();
    int face_count = (int)triangles - degenerate;
    int euler = welded_v - edge_count + face_count;

    st.welded_vertices = welded_v;
    st.edges = edge_count;
    st.nondegenerate_faces = face_count;
    st.degenerate = degenerate;
    st.euler = euler;
    st.bad_edges = bad_edges;
    st.volume = volume;
    rep.total_triangles += triangles;
    rep.total_degenerate += degenerate;
    rep.total_bad_edges += bad_edges;
    rep.min_volume = std::min(rep.min_volume, volume);

    if (bad_edges != 0) {
        rep.fail("closed-topology",
                 "%s sub=%d: %d geometric edges lack exactly two opposite faces",
                 c.name, c.subdivisions, bad_edges);
    }
    if (euler != 2) {
        rep.fail("closed-topology",
                 "%s sub=%d: single-shell Euler characteristic %d != 2 "
                 "(V=%d E=%d F=%d)",
                 c.name, c.subdivisions, euler, welded_v, edge_count, face_count);
    }
    if (!(volume > 0.0)) {
        rep.fail("closed-topology", "%s sub=%d: closed signed volume %.9g <= 0",
                 c.name, c.subdivisions, volume);
    }
}

/* The null normals / null texcoords path must produce identical positions and
 * a fully equivalent index buffer. */
void check_attribute_equivalence(const ShapeConfig& c, const Mesh& full,
                                 const Mesh& bare, Report& rep) {
    if (full.num_vertices != bare.num_vertices ||
        full.num_indices != bare.num_indices) {
        rep.fail("attribute-equivalence",
                 "%s sub=%d: no-attribute path changed counts (v %u/%u i %u/%u)",
                 c.name, c.subdivisions, full.num_vertices, bare.num_vertices,
                 full.num_indices, bare.num_indices);
        return;
    }
    const float* pa = full.positions.data() + 3;
    const float* pb = bare.positions.data() + 3;
    if (memcmp(pa, pb, 3 * full.num_vertices * sizeof(float)) != 0) {
        rep.fail("attribute-equivalence",
                 "%s sub=%d: positions differ when normals/texcoords are null",
                 c.name, c.subdivisions);
    }
    for (uint32_t i = 0; i < full.num_indices; ++i) {
        if (index_of(full, i) != index_of(bare, i)) {
            rep.fail("attribute-equivalence",
                     "%s sub=%d: index %u differs when attributes are null",
                     c.name, c.subdivisions, i);
            break;
        }
    }
}

void write_obj(const std::string& path, const Mesh& m) {
    std::ofstream obj(path);
    obj << "# par_octasphere geometric regression sample (rounded box)\n";
    for (uint32_t v = 0; v < m.num_vertices; ++v) {
        const float* p = position_of(m, v);
        obj << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
    }
    for (uint32_t v = 0; v < m.num_vertices; ++v) {
        const float* n = normal_of(m, v);
        obj << "vn " << n[0] << ' ' << n[1] << ' ' << n[2] << '\n';
    }
    for (uint32_t tri = 0; tri < m.num_indices / 3; ++tri) {
        uint16_t a = index_of(m, tri * 3) + 1;
        uint16_t b = index_of(m, tri * 3 + 1) + 1;
        uint16_t cc = index_of(m, tri * 3 + 2) + 1;
        obj << "f " << a << "//" << a << ' ' << b << "//" << b << ' ' << cc
            << "//" << cc << '\n';
    }
}

const ShapeConfig kScenarios[] = {
    {"sphere",    1.0f,  0.0f,  0.0f,  0.0f, 0, true},
    {"sphere",    1.0f,  0.0f,  0.0f,  0.0f, 2, true},
    {"sphere",    1.0f,  0.0f,  0.0f,  0.0f, 5, true},
    {"sphere-big", 2.5f, 0.0f,  0.0f,  0.0f, 2, true},
    {"capsule-y", 0.5f,  0.0f,  4.0f,  0.0f, 0, false},
    {"capsule-y", 0.5f,  0.0f,  4.0f,  0.0f, 2, false},
    {"capsule-y", 0.5f,  0.0f,  4.0f,  0.0f, 5, false},
    {"capsule-x", 1.0f,  5.0f,  0.0f,  0.0f, 2, false},
    {"capsule-z", 0.75f, 0.0f,  0.0f,  3.5f, 2, false},
    {"rounded-box", 0.5f, 3.0f, 4.0f, 5.0f, 0, false},
    {"rounded-box", 0.5f, 3.0f, 4.0f, 5.0f, 2, false},
    {"rounded-box", 0.5f, 3.0f, 4.0f, 5.0f, 5, false},
    {"rounded-box-big", 2.0f, 12.0f, 16.0f, 20.0f, 2, false},
    // Width 1.5 is below the 2r = 2 diameter and must be clamped to it.
    {"clamped-diameter", 1.0f, 1.5f, 4.0f, 5.0f, 2, false},
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <output_dir> <clean|angle|offset|winding>\n",
                     argv[0]);
        return 2;
    }
    std::string output_dir = argv[1];
    std::string mode = argv[2];
    std::string expected_family;
    if (mode == "clean") {
        expected_family = "";
    } else if (mode == "angle") {
        expected_family = "geodesic-sampling";
    } else if (mode == "offset") {
        expected_family = "surface-normal";
    } else if (mode == "winding") {
        expected_family = "closed-topology";
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }

    std::printf("# octasphere geometry mode=%s PARO_CONSTANT_TOPOLOGY=%d scenarios=%zu\n",
                mode.c_str(), (int)PARO_CONSTANT_TOPOLOGY,
                sizeof(kScenarios) / sizeof(kScenarios[0]));

    Report report;
    const Mesh* obj_mesh = nullptr;
    Mesh obj_mesh_storage;

    // All scenarios generate first; assertions run after generation completes.
    std::vector<Mesh> full_meshes;
    std::vector<Mesh> bare_meshes;
    full_meshes.reserve(sizeof(kScenarios) / sizeof(kScenarios[0]));
    bare_meshes.reserve(sizeof(kScenarios) / sizeof(kScenarios[0]));
    for (const ShapeConfig& scenario : kScenarios) {
        full_meshes.push_back(generate_mesh(scenario, true, report));
        bare_meshes.push_back(generate_mesh(scenario, false, report));
    }

    for (size_t i = 0; i < sizeof(kScenarios) / sizeof(kScenarios[0]); ++i) {
        const ShapeConfig& scenario = kScenarios[i];
        const Mesh& full = full_meshes[i];
        const Mesh& bare = bare_meshes[i];
        ConfigStats stats;
        stats.name = scenario.name;
        stats.subdivisions = scenario.subdivisions;
        stats.capacity_vertices = full.capacity_vertices;
        stats.capacity_indices = full.capacity_indices;
        stats.real_vertices = full.num_vertices;
        stats.real_indices = full.num_indices;

        check_attribute_equivalence(scenario, full, bare, report);
        check_surface_and_normals(scenario, full, report, stats);
        check_geodesic_sampling(scenario, full, report, stats);
        check_closed_topology(scenario, full, report, stats);
        report.stats.push_back(stats);

        if (std::string(scenario.name) == "rounded-box" && scenario.subdivisions == 2) {
            obj_mesh_storage = full;
            obj_mesh = &obj_mesh_storage;
        }
    }

    if (obj_mesh) {
        write_obj(output_dir + "/rounded_box.obj", *obj_mesh);
    } else {
        report.fail("bookkeeping", "rounded box OBJ scenario missing");
    }

    std::ofstream stats_file(output_dir + "/geometry-stats.txt");
    stats_file << "mode=" << mode << " PARO_CONSTANT_TOPOLOGY="
               << (int)PARO_CONSTANT_TOPOLOGY << '\n';
    stats_file << "name sub capV capI realV realI weldV E F deg euler badEdge "
                  "volume worstDistErr worstNormalErr worstSlerpErr\n";
    for (const ConfigStats& s : report.stats) {
        stats_file << s.name << ' ' << s.subdivisions << ' ' << s.capacity_vertices
                   << ' ' << s.capacity_indices << ' ' << s.real_vertices << ' '
                   << s.real_indices << ' ' << s.welded_vertices << ' ' << s.edges
                   << ' ' << s.nondegenerate_faces << ' ' << s.degenerate << ' '
                   << s.euler << ' ' << s.bad_edges << ' ' << s.volume << ' '
                   << s.worst_distance_error << ' ' << s.worst_normal_error << ' '
                   << s.worst_slerp_error << '\n';
    }
    stats_file << "totals triangles=" << report.total_triangles
               << " degenerate=" << report.total_degenerate
               << " badEdges=" << report.total_bad_edges
               << " minVolume=" << report.min_volume
               << " worstDistanceError=" << report.worst_distance_error
               << " worstNormalError=" << report.worst_normal_error
               << " worstSlerpError=" << report.worst_slerp_error << '\n';

    for (const Failure& failure : report.failures) {
        std::printf("FAIL [%s] %s\n", failure.family.c_str(), failure.detail.c_str());
    }

    int exit_code = 0;
    if (mode == "clean") {
        bool surface_ok = !report.family_failed("surface-normal");
        bool geodesic_ok = !report.family_failed("geodesic-sampling");
        bool topology_ok = !report.family_failed("closed-topology");
        bool bookkeeping_ok = !report.family_failed("bookkeeping") &&
                              !report.family_failed("attribute-equivalence");
        if (surface_ok) {
            std::printf("PASS surface-normal (worst dist err %.3g, worst normal L1 %.3g)\n",
                        report.worst_distance_error, report.worst_normal_error);
        }
        if (geodesic_ok) {
            std::printf("PASS geodesic-sampling (worst slerp L1 %.3g)\n",
                        report.worst_slerp_error);
        }
        if (topology_ok) {
            std::printf("PASS closed-topology (triangles %ld, degenerate %ld, "
                        "bad edges %ld, min volume %.6g)\n",
                        report.total_triangles, report.total_degenerate,
                        report.total_bad_edges, report.min_volume);
        }
        if (bookkeeping_ok) {
            std::printf("PASS bookkeeping (sentinels, capacities, attribute equivalence)\n");
        }
        if (!report.failures.empty()) {
            std::printf("GEOMETRY REGRESSION: %zu assertion(s) failed\n",
                        report.failures.size());
            exit_code = 1;
        }
    } else {
        if (!report.family_failed(expected_family)) {
            std::printf("ESCAPE: %s mutation did not trigger %s assertions\n",
                        mode.c_str(), expected_family.c_str());
            exit_code = 1;
        } else {
            for (const Failure& failure : report.failures) {
                if (failure.family == expected_family) {
                    std::printf("DETECTED %s: %s\n", mode.c_str(),
                                failure.detail.c_str());
                    break;
                }
            }
            std::printf("DETECTED %s (family=%s, observed failures=%zu)\n",
                        mode.c_str(), expected_family.c_str(), report.failures.size());
        }
    }
    return exit_code;
}
