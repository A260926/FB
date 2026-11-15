// Regression tests for the inverse-lighting (emission fit) entry point.
// Registered with the task driver in three groups: geometry, inverse, render.

#include "task_support/check.h"

#include "Camera.h"
#include "PatchTransport.h"
#include "object/ObjectList.h"
#include "object/PatchMesh.h"
#include "object/Sphere.h"
#include "object/Triangle.h"

#include <array>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- helpers

// Dense Gaussian elimination with partial pivoting; the test's own
// independent linear solver. Returns false on singularity.
bool dense_solve(std::vector<double> A, int n, std::vector<double> b,
                 std::vector<double>& x) {
    for (int k = 0; k < n; ++k) {
        int p = k;
        for (int i = k + 1; i < n; ++i)
            if (std::fabs(A[i * n + k]) > std::fabs(A[p * n + k])) p = i;
        if (std::fabs(A[p * n + k]) < 1e-290) return false;
        if (p != k) {
            for (int j = 0; j < n; ++j) std::swap(A[p * n + j], A[k * n + j]);
            std::swap(b[p], b[k]);
        }
        for (int i = k + 1; i < n; ++i) {
            const double f = A[i * n + k] / A[k * n + k];
            for (int j = k; j < n; ++j) A[i * n + j] -= f * A[k * n + j];
            b[i] -= f * b[k];
        }
    }
    x.assign(n, 0.0);
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int j = i + 1; j < n; ++j) s -= A[i * n + j] * x[j];
        x[i] = s / A[i * n + i];
    }
    return true;
}

struct RefFace {
    std::array<Point3, 3> v;
    Vector3 n;
    double area;
};

RefFace ref_face(const std::array<Point3, 3>& v) {
    RefFace f;
    f.v = v;
    const Vector3 n = cross(v[1] - v[0], v[2] - v[0]);
    const double len = n.length();
    f.n = n / len;
    f.area = 0.5 * len;
    return f;
}

// Independent reference implementation of the discrete transport spec:
// three sample points per face at the cyclic permutations of barycentric
// (2/3, 1/6, 1/6), nine connecting segments per face pair tested against
// the world on (eps, dist - eps), global conservation scaling.
std::vector<double> reference_form_factors(const std::vector<RefFace>& faces,
                                           const Object& world, double eps,
                                           double& s_out) {
    const int n = static_cast<int>(faces.size());
    static const double w[3][3] = {
        {2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0},
        {1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0},
        {1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0}};
    std::vector<std::array<Point3, 3>> sp(n);
    for (int i = 0; i < n; ++i)
        for (int a = 0; a < 3; ++a)
            sp[i][a] = w[a][0] * faces[i].v[0] + w[a][1] * faces[i].v[1] +
                       w[a][2] * faces[i].v[2];

    std::vector<double> H(n * n, 0.0);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            double sum = 0.0;
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    const Vector3 seg = sp[j][b] - sp[i][a];
                    const double dist = seg.length();
                    if (dist <= 100.0 * eps) continue;
                    const Vector3 d = seg / dist;
                    const double ci = dot(faces[i].n, d);
                    const double cj = dot(faces[j].n, -d);
                    const double term = std::max(0.0, ci) * std::max(0.0, cj);
                    if (term == 0.0) continue;
                    Ray ray(sp[i][a], d);
                    IntersectionRecord rec;
                    if (world.intersect(ray, Interval(eps, dist - eps), rec))
                        continue;
                    sum += term / (dist * dist);
                }
            }
            const double hij = faces[i].area * faces[j].area / (9.0 * pi) * sum;
            H[i * n + j] = hij;
            H[j * n + i] = hij;
        }
    }
    double s = 0.0;
    for (int i = 0; i < n; ++i) {
        double row = 0.0;
        for (int j = 0; j < n; ++j) row += H[i * n + j] / faces[i].area;
        s = std::max(s, row);
    }
    s_out = s;
    const double scale = std::max(1.0, s);
    std::vector<double> F(n * n, 0.0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) F[i * n + j] = H[i * n + j] / scale / faces[i].area;
    return F;
}

// Independent per-channel response: invert (I - diag(rho) F) rebuilt from
// the transport's read-only queries with the test's own solver.
std::vector<double> reference_response(const PatchTransport& t, int channel) {
    const int n = t.face_count();
    std::vector<double> M(n * n, 0.0), inv(n * n, 0.0);
    for (int i = 0; i < n; ++i) {
        M[i * n + i] = 1.0;
        for (int j = 0; j < n; ++j)
            M[i * n + j] -= t.reflectance(i)[channel] * t.form_factor(i, j);
    }
    for (int col = 0; col < n; ++col) {
        std::vector<double> b(n, 0.0), x;
        b[col] = 1.0;
        dense_solve(M, n, b, x);
        for (int i = 0; i < n; ++i) inv[i * n + col] = x[i];
    }
    return inv;
}

// Enumerate all active sets for min sum_o w_o (Gx - t)^2 + lambda |x|^2,
// x >= 0, and return the best feasible KKT point. Independent check of the
// production solver.
struct KktResult {
    bool found = false;
    double objective = 0.0;
    std::vector<double> x;
};

KktResult kkt_enumerate(const std::vector<double>& G, int ocount, int m,
                        const std::vector<double>& w, const std::vector<double>& t,
                        double lambda) {
    // Normal-equation form: grad = H x - c with H = 2 G'WG + 2 lambda I.
    std::vector<double> H(m * m, 0.0), c(m, 0.0);
    for (int o = 0; o < ocount; ++o) {
        for (int k = 0; k < m; ++k) {
            c[k] += 2.0 * w[o] * G[o * m + k] * t[o];
            for (int l = 0; l < m; ++l)
                H[k * m + l] += 2.0 * w[o] * G[o * m + k] * G[o * m + l];
        }
    }
    for (int k = 0; k < m; ++k) H[k * m + k] += 2.0 * lambda;

    auto objective_of = [&](const std::vector<double>& x) {
        double obj = 0.0;
        for (int o = 0; o < ocount; ++o) {
            double r = -t[o];
            for (int k = 0; k < m; ++k) r += G[o * m + k] * x[k];
            obj += w[o] * r * r;
        }
        for (int k = 0; k < m; ++k) obj += lambda * x[k] * x[k];
        return obj;
    };

    KktResult best;
    best.objective = 1e300;
    for (int mask = 0; mask < (1 << m); ++mask) {
        std::vector<int> S;
        for (int k = 0; k < m; ++k)
            if (mask & (1 << k)) S.push_back(k);
        const int p = static_cast<int>(S.size());
        std::vector<double> x(m, 0.0);
        if (p > 0) {
            std::vector<double> Hs(p * p), cs(p);
            for (int a = 0; a < p; ++a) {
                cs[a] = c[S[a]];
                for (int b = 0; b < p; ++b) Hs[a * p + b] = H[S[a] * m + S[b]];
            }
            std::vector<double> xs;
            if (!dense_solve(Hs, p, cs, xs)) continue;
            bool ok = true;
            for (int a = 0; a < p; ++a)
                if (xs[a] < -1e-9) { ok = false; break; }
            if (!ok) continue;
            for (int a = 0; a < p; ++a) x[S[a]] = std::max(0.0, xs[a]);
        }
        std::vector<double> g(m, 0.0);
        for (int i = 0; i < m; ++i) {
            double s = -c[i];
            for (int j = 0; j < m; ++j) s += H[i * m + j] * x[j];
            g[i] = s;
        }
        bool kkt = true;
        for (int k = 0; k < m; ++k) {
            if (x[k] == 0.0) {
                if (g[k] < -1e-7) { kkt = false; break; }
            } else if (std::fabs(g[k]) > 1e-7) {
                kkt = false;
                break;
            }
        }
        if (!kkt) continue;
        const double obj = objective_of(x);
        if (obj < best.objective) {
            best.objective = obj;
            best.x = x;
            best.found = true;
        }
    }
    return best;
}

bool read_ppm(const std::string& path, int& w, int& h, std::vector<int>& values) {
    std::ifstream f(path);
    if (!f) return false;
    std::string magic;
    int maxv = 0;
    f >> magic >> w >> h >> maxv;
    if (magic != "P3" || maxv != 255 || w <= 0 || h <= 0) return false;
    values.clear();
    int v;
    while (f >> v) values.push_back(v);
    return values.size() == static_cast<size_t>(w) * h * 3;
}

// Framebuffer pixel (i, j) from PPM values (writer mirrors columns).
std::array<int, 3> ppm_pixel(const std::vector<int>& values, int w, int i, int j) {
    const size_t base = (static_cast<size_t>(j) * w + (w - 1 - i)) * 3;
    return {{values[base], values[base + 1], values[base + 2]}};
}

int tone_byte(const Camera& cam, const Radiance& hdr, int channel) {
    const Radiance mapped = cam.tone_map(hdr);
    const double c = std::fmax(0.0, std::fmin(1.0, mapped[channel]));
    return static_cast<int>(255.999 * c);
}

template <typename F>
bool throws_invalid_argument(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// ------------------------------------------------------------ geometry group

// A quad PatchMesh plus sphere/triangle occluders in one ObjectList: the
// nearest hit carries t, normal, material, the source mesh and the source
// face number; regular primitives report no patch identity.
int test_mesh_intersection_identity() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    std::vector<Point3> verts = {
        Point3(-1, -1, -3), Point3(1, -1, -3), Point3(1, 1, -3), Point3(-1, 1, -3)};
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{0, 2, 3}}};
    std::vector<Material*> mats = {&mat, &mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);

    auto far_sphere = std::make_shared<Sphere>(Point3(5, 0, -6), 1.0, &mat);
    auto blocker = std::make_shared<Sphere>(Point3(0, 0, -1.5), 0.15, &mat);
    auto tri = std::make_shared<Triangle>(Point3(-5, -1, -6), Point3(-4, -1, -6),
                                          Point3(-4.5, 1, -6), &mat);

    ObjectList world;
    world.add(mesh);
    world.add(far_sphere);
    world.add(blocker);
    world.add(tri);
    world.build();

    IntersectionRecord rec;

    // Front-face hit on source face 0 of the mesh.
    Ray r0(Point3(0, 0, 0), unit_vector(Vector3(0.3, -0.3, -3)));
    TASK_CHECK(world.intersect(r0, Interval(0.001, 100), rec));
    TASK_CHECK(std::fabs(rec.t - Vector3(0.3, -0.3, -3).length()) < 1e-9);
    TASK_CHECK(rec.front_face);
    TASK_CHECK(rec.mat_ptr == &mat);
    TASK_CHECK(rec.patch_mesh == mesh.get());
    TASK_CHECK(rec.patch_face == 0);
    TASK_CHECK(std::fabs(rec.normal.z() - 1.0) < 1e-9);

    // Front-face hit on source face 1.
    Ray r1(Point3(0, 0, 0), unit_vector(Vector3(-0.3, 0.5, -3)));
    TASK_CHECK(world.intersect(r1, Interval(0.001, 100), rec));
    TASK_CHECK(rec.patch_mesh == mesh.get());
    TASK_CHECK(rec.patch_face == 1);

    // The same identities come out with the linear (non-BVH) path.
    world.use_bvh = UseBVH::DISABLE;
    TASK_CHECK(world.intersect(r0, Interval(0.001, 100), rec));
    TASK_CHECK(rec.patch_mesh == mesh.get());
    TASK_CHECK(rec.patch_face == 0);
    TASK_CHECK(world.intersect(r1, Interval(0.001, 100), rec));
    TASK_CHECK(rec.patch_face == 1);
    world.use_bvh = UseBVH::ENABLE;

    // Regular primitives carry no patch identity.
    Ray rs(Point3(0, 0, 0), unit_vector(Vector3(5, 0, -6)));
    TASK_CHECK(world.intersect(rs, Interval(0.001, 100), rec));
    TASK_CHECK(rec.patch_mesh == nullptr);
    TASK_CHECK(rec.patch_face == -1);
    TASK_CHECK(rec.mat_ptr == &mat);

    Ray rt(Point3(-4.5, 0, 0), Vector3(0, 0, -1));
    TASK_CHECK(world.intersect(rt, Interval(0.001, 100), rec));
    TASK_CHECK(rec.patch_mesh == nullptr);
    TASK_CHECK(rec.patch_face == -1);

    // Mixed occlusion: the sphere in front of the quad wins the nearest hit.
    Ray rb(Point3(0, 0, 0), Vector3(0, 0, -1));
    TASK_CHECK(world.intersect(rb, Interval(0.001, 100), rec));
    TASK_CHECK(rec.patch_mesh == nullptr);
    TASK_CHECK(std::fabs(rec.t - 1.35) < 1e-9);

    // Miss.
    Ray rm(Point3(0, 0, 0), Vector3(0, 1, 0));
    TASK_CHECK(!world.intersect(rm, Interval(0.001, 100), rec));
    return 0;
}

// Source face numbers follow the construction order no matter how the
// internal acceleration reorders the faces.
int test_source_face_stable() {
    Material mat;
    std::vector<Point3> verts;
    std::vector<std::array<int, 3>> tris;
    std::vector<Material*> mats;
    for (int k = 0; k < 5; ++k) {
        const double x = k * 1.5;
        verts.push_back(Point3(x, 0, -3));
        verts.push_back(Point3(x + 0.5, 0, -3));
        verts.push_back(Point3(x + 0.25, 0.5, -3));
        tris.push_back({{3 * k, 3 * k + 1, 3 * k + 2}});
        mats.push_back(&mat);
    }
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);
    ObjectList world;
    world.add(mesh);
    world.build();

    for (int k = 0; k < 5; ++k) {
        Ray r(Point3(k * 1.5 + 0.25, 0.2, 0), Vector3(0, 0, -1));
        IntersectionRecord rec;
        TASK_CHECK(world.intersect(r, Interval(0.001, 100), rec));
        TASK_CHECK(rec.patch_mesh == mesh.get());
        TASK_CHECK(rec.patch_face == k);
        TASK_CHECK(std::fabs(rec.t - 3.0) < 1e-9);
    }
    world.use_bvh = UseBVH::DISABLE;
    for (int k = 0; k < 5; ++k) {
        Ray r(Point3(k * 1.5 + 0.25, 0.2, 0), Vector3(0, 0, -1));
        IntersectionRecord rec;
        TASK_CHECK(world.intersect(r, Interval(0.001, 100), rec));
        TASK_CHECK(rec.patch_face == k);
    }
    return 0;
}

// Two facing triangles: form factors match an independent nine-point
// reference, area reciprocity holds, and face identity queries line up.
int test_form_factor_reference() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    std::vector<Point3> verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),   // face 0, +z
        Point3(0, 0, 1), Point3(0, 1, 1), Point3(1, 0, 1)};  // face 1, -z
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> mats = {&mat, &mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);

    ObjectList world;
    world.add(mesh);
    world.build();

    PatchTransport transport({mesh}, world, 1e-6);
    TASK_CHECK(transport.face_count() == 2);
    TASK_CHECK(std::fabs(transport.area(0) - 0.5) < 1e-12);
    TASK_CHECK(std::fabs(transport.area(1) - 0.5) < 1e-12);
    TASK_CHECK(std::fabs(transport.reflectance(0).x() - 0.45) < 1e-12);
    TASK_CHECK(transport.face_mesh(0) == mesh.get());
    TASK_CHECK(transport.face_mesh(1) == mesh.get());
    TASK_CHECK(transport.face_source_index(0) == 0);
    TASK_CHECK(transport.face_source_index(1) == 1);
    TASK_CHECK(transport.form_factor(0, 0) == 0.0);
    TASK_CHECK(transport.form_factor(1, 1) == 0.0);

    std::vector<RefFace> faces = {
        ref_face({{verts[0], verts[1], verts[2]}}),
        ref_face({{verts[3], verts[4], verts[5]}})};
    double s_ref = 0.0;
    std::vector<double> F_ref = reference_form_factors(faces, world, 1e-6, s_ref);
    TASK_CHECK(s_ref < 1.0); // no conservation scaling in this configuration
    TASK_CHECK(F_ref[1] > 0.0);
    TASK_CHECK(std::fabs(transport.form_factor(0, 1) - F_ref[1]) < 1e-10);
    TASK_CHECK(std::fabs(transport.form_factor(1, 0) - F_ref[2]) < 1e-10);

    // Area reciprocity: A0 F01 = A1 F10.
    TASK_CHECK(std::fabs(transport.area(0) * transport.form_factor(0, 1) -
                         transport.area(1) * transport.form_factor(1, 0)) < 1e-12);
    return 0;
}

// A close aligned pair makes the unscaled row sums exceed 1, forcing the
// single global conservation scaling; a side wall keeps the row sums
// unequal, so per-row normalization would be visibly different.
int test_form_factor_scaling() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    std::vector<Point3> verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),              // 0: +z
        Point3(0, 0, 0.02), Point3(0, 1, 0.02), Point3(1, 0, 0.02),     // 1: -z, aligned
        Point3(2, 0, 0), Point3(2, 0, 1), Point3(2, 1, 0)};             // 2: -x wall
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}, {{6, 7, 8}}};
    std::vector<Material*> mats = {&mat, &mat, &mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);

    ObjectList world;
    world.add(mesh);
    world.build();

    const double eps = 1e-6;
    PatchTransport transport({mesh}, world, eps);
    TASK_CHECK(transport.face_count() == 3);

    std::vector<RefFace> faces = {
        ref_face({{verts[0], verts[1], verts[2]}}),
        ref_face({{verts[3], verts[4], verts[5]}}),
        ref_face({{verts[6], verts[7], verts[8]}})};
    double s_ref = 0.0;
    std::vector<double> F_ref = reference_form_factors(faces, world, eps, s_ref);
    TASK_CHECK(s_ref > 1.0); // scaling is active in this configuration

    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            TASK_CHECK(std::fabs(transport.form_factor(i, j) - F_ref[i * 3 + j]) < 1e-10);

    // Row sums are conserved (<= 1) but NOT each normalized to 1: the same
    // global scale max(1, s) divides every row.
    const double scale = std::max(1.0, s_ref);
    bool any_below_one = false;
    for (int i = 0; i < 3; ++i) {
        double row = 0.0, s_i = 0.0;
        for (int j = 0; j < 3; ++j) {
            row += transport.form_factor(i, j);
            s_i += F_ref[i * 3 + j] * scale;
        }
        TASK_CHECK(row <= 1.0 + 1e-12);
        TASK_CHECK(std::fabs(row - s_i / scale) < 1e-10);
        if (row < 1.0 - 1e-6) any_below_one = true;
    }
    TASK_CHECK(any_below_one);

    // Area reciprocity between every pair.
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            TASK_CHECK(std::fabs(transport.area(i) * transport.form_factor(i, j) -
                                 transport.area(j) * transport.form_factor(j, i)) < 1e-12);
    return 0;
}

// Faces of different areas: form factors match the independent reference
// and area reciprocity A_i F_ij = A_j F_ji holds.
int test_form_factor_areas() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    std::vector<Point3> verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),       // area 0.5
        Point3(0, 0, 1), Point3(0, 2, 1), Point3(2, 0, 1)};      // area 2.0
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> mats = {&mat, &mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);

    ObjectList world;
    world.add(mesh);
    world.build();

    PatchTransport transport({mesh}, world, 1e-6);
    TASK_CHECK(std::fabs(transport.area(0) - 0.5) < 1e-12);
    TASK_CHECK(std::fabs(transport.area(1) - 2.0) < 1e-12);

    std::vector<RefFace> faces = {
        ref_face({{verts[0], verts[1], verts[2]}}),
        ref_face({{verts[3], verts[4], verts[5]}})};
    double s_ref = 0.0;
    std::vector<double> F_ref = reference_form_factors(faces, world, 1e-6, s_ref);
    TASK_CHECK(s_ref < 1.0); // no scaling here
    TASK_CHECK(F_ref[1] > 0.0);
    TASK_CHECK(std::fabs(transport.form_factor(0, 1) - F_ref[1]) < 1e-10);
    TASK_CHECK(std::fabs(transport.form_factor(1, 0) - F_ref[2]) < 1e-10);

    // Reciprocity is meaningful exactly because the areas differ.
    TASK_CHECK(std::fabs(transport.area(0) * transport.form_factor(0, 1) -
                         transport.area(1) * transport.form_factor(1, 0)) < 1e-12);
    TASK_CHECK(transport.form_factor(0, 1) != transport.form_factor(1, 0));
    return 0;
}

// Back faces do not transmit: a pair whose normals face away gets F = 0,
// while the same geometry facing each other transfers energy.
int test_backface_no_transfer() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    // Face 1 wound so its normal (+z) points away from face 0.
    std::vector<Point3> verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),
        Point3(0, 0, 1), Point3(1, 0, 1), Point3(0, 1, 1)};
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> mats = {&mat, &mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);

    ObjectList world;
    world.add(mesh);
    world.build();

    PatchTransport transport({mesh}, world, 1e-6);
    TASK_CHECK(transport.form_factor(0, 1) == 0.0);
    TASK_CHECK(transport.form_factor(1, 0) == 0.0);

    // Control: same positions, face 1 wound to face face 0.
    std::vector<std::array<int, 3>> tris2 = {{{0, 1, 2}}, {{3, 5, 4}}};
    auto mesh2 = std::make_shared<PatchMesh>(verts, tris2, mats);
    ObjectList world2;
    world2.add(mesh2);
    world2.build();
    PatchTransport transport2({mesh2}, world2, 1e-6);
    TASK_CHECK(transport2.form_factor(0, 1) > 0.0);
    TASK_CHECK(transport2.form_factor(1, 0) > 0.0);
    return 0;
}

// Occlusion by another face of the same mesh, by a regular Triangle and by
// a Sphere all zero the corresponding form factors.
int test_occlusion_variants() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    std::vector<Point3> base_verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),       // 0: +z
        Point3(0, 0, 1), Point3(0, 1, 1), Point3(1, 0, 1)};      // 1: -z
    std::vector<std::array<int, 3>> base_tris = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> mats2 = {&mat, &mat};

    // (a) Blocker is another face of the same mesh (wound to face A).
    {
        std::vector<Point3> verts = base_verts;
        verts.push_back(Point3(-3, -3, 0.5));
        verts.push_back(Point3(0, 3, 0.5));
        verts.push_back(Point3(3, -3, 0.5)); // normal -z, toward face 0
        std::vector<std::array<int, 3>> tris = base_tris;
        tris.push_back({{6, 7, 8}});
        std::vector<Material*> mats = {&mat, &mat, &mat};
        auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);
        ObjectList world;
        world.add(mesh);
        world.build();
        PatchTransport transport({mesh}, world, 1e-6);
        TASK_CHECK(transport.face_count() == 3);
        TASK_CHECK(transport.form_factor(0, 1) == 0.0);
        TASK_CHECK(transport.form_factor(1, 0) == 0.0);
        TASK_CHECK(transport.form_factor(0, 2) > 0.0);  // A sees the blocker's front
        TASK_CHECK(transport.form_factor(1, 2) == 0.0); // B sees its back: no transfer
    }

    // (b) Blocker is a regular Triangle primitive.
    {
        auto mesh = std::make_shared<PatchMesh>(base_verts, base_tris, mats2);
        auto blocker = std::make_shared<Triangle>(
            Point3(-3, -3, 0.5), Point3(3, -3, 0.5), Point3(0, 3, 0.5), &mat);
        ObjectList world;
        world.add(mesh);
        world.add(blocker);
        world.build();
        PatchTransport transport({mesh}, world, 1e-6);
        TASK_CHECK(transport.form_factor(0, 1) == 0.0);
    }

    // (c) Blocker is a Sphere.
    {
        auto mesh = std::make_shared<PatchMesh>(base_verts, base_tris, mats2);
        auto blocker = std::make_shared<Sphere>(Point3(0.4, 0.4, 0.5), 0.45, &mat);
        ObjectList world;
        world.add(mesh);
        world.add(blocker);
        world.build();
        PatchTransport transport({mesh}, world, 1e-6);
        TASK_CHECK(transport.form_factor(0, 1) == 0.0);
    }

    // (d) Unblocked control: the same pair exchanges energy.
    {
        auto mesh = std::make_shared<PatchMesh>(base_verts, base_tris, mats2);
        ObjectList world;
        world.add(mesh);
        world.build();
        PatchTransport transport({mesh}, world, 1e-6);
        TASK_CHECK(transport.form_factor(0, 1) > 0.0);
    }
    return 0;
}

// Sample-point pairs no farther apart than 100 eps contribute zero.
int test_eps_distance_rule() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);

    auto build = [&](double gap, std::shared_ptr<PatchMesh>& mesh, ObjectList& world) {
        std::vector<Point3> verts = {
            Point3(0, 0, 0), Point3(0.001, 0, 0), Point3(0, 0.001, 0),
            Point3(0, 0, gap), Point3(0, 0.001, gap), Point3(0.001, 0, gap)};
        std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}};
        std::vector<Material*> mats = {&mat, &mat};
        mesh = std::make_shared<PatchMesh>(verts, tris, mats);
        world.add(mesh);
        world.build();
    };

    const double eps = 1e-3;
    {
        std::shared_ptr<PatchMesh> mesh;
        ObjectList world;
        build(0.05, mesh, world); // 0.05 <= 100 * eps
        PatchTransport transport({mesh}, world, eps);
        TASK_CHECK(transport.form_factor(0, 1) == 0.0);
        TASK_CHECK(transport.form_factor(1, 0) == 0.0);
    }
    {
        std::shared_ptr<PatchMesh> mesh;
        ObjectList world;
        build(0.5, mesh, world); // 0.5 > 100 * eps
        PatchTransport transport({mesh}, world, eps);
        TASK_CHECK(transport.form_factor(0, 1) > 0.0);
        TASK_CHECK(transport.form_factor(1, 0) > 0.0);
    }
    return 0;
}

// Invalid geometry and transport configurations are rejected.
int test_invalid_construction() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);
    std::vector<Point3> verts = {Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0)};
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}};
    std::vector<Material*> mats = {&mat};

    // Degenerate (collinear) triangle.
    {
        std::vector<Point3> v = {Point3(0, 0, 0), Point3(1, 0, 0), Point3(2, 0, 0)};
        TASK_CHECK(throws_invalid_argument([&] { PatchMesh m(v, tris, mats); }));
    }
    // Index out of range.
    {
        std::vector<std::array<int, 3>> bad = {{{0, 1, 3}}};
        TASK_CHECK(throws_invalid_argument([&] { PatchMesh m(verts, bad, mats); }));
    }
    // Material count mismatch.
    {
        std::vector<Material*> two = {&mat, &mat};
        TASK_CHECK(throws_invalid_argument([&] { PatchMesh m(verts, tris, two); }));
    }
    // Null material.
    {
        std::vector<Material*> nullm = {nullptr};
        TASK_CHECK(throws_invalid_argument([&] { PatchMesh m(verts, tris, nullm); }));
    }
    // Non-finite vertex.
    {
        std::vector<Point3> v = {Point3(0, 0, 0), Point3(1, 0, 0),
                                 Point3(0, infinity, 0)};
        TASK_CHECK(throws_invalid_argument([&] { PatchMesh m(v, tris, mats); }));
    }
    // Empty triangle list.
    {
        std::vector<std::array<int, 3>> none;
        std::vector<Material*> nomats;
        TASK_CHECK(throws_invalid_argument([&] { PatchMesh m(verts, none, nomats); }));
    }

    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);
    ObjectList world;
    world.add(mesh);
    world.build();

    // Bad eps values.
    TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({mesh}, world, 0.0); }));
    TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({mesh}, world, -1.0); }));
    TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({mesh}, world, infinity); }));
    // Empty mesh list.
    TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({}, world, 1e-6); }));
    // Duplicate mesh.
    TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({mesh, mesh}, world, 1e-6); }));
    // Too many faces.
    {
        std::vector<Point3> v;
        std::vector<std::array<int, 3>> t129;
        std::vector<Material*> m129;
        for (int k = 0; k < 129; ++k) {
            v.push_back(Point3(3.0 * k, 0, 0));
            v.push_back(Point3(3.0 * k + 1, 0, 0));
            v.push_back(Point3(3.0 * k, 1, 0));
            t129.push_back({{3 * k, 3 * k + 1, 3 * k + 2}});
            m129.push_back(&mat);
        }
        auto big = std::make_shared<PatchMesh>(v, t129, m129);
        ObjectList w2;
        w2.add(big);
        w2.build();
        TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({big}, w2, 1e-6); }));
    }
    // rho outside [0, 0.95].
    {
        Material hot;
        hot.kd = 1.0;
        hot.diffusecolor = Radiance(1.0, 1.0, 1.0); // rho = 1.0 > 0.95
        std::vector<Material*> hm = {&hot};
        auto m2 = std::make_shared<PatchMesh>(verts, tris, hm);
        ObjectList w2;
        w2.add(m2);
        w2.build();
        TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({m2}, w2, 1e-6); }));
    }
    // Textured / reflective / refractive materials are unsupported.
    {
        Material tex;
        tex.has_texture = true;
        std::vector<Material*> tm = {&tex};
        auto m2 = std::make_shared<PatchMesh>(verts, tris, tm);
        ObjectList w2;
        w2.add(m2);
        w2.build();
        TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({m2}, w2, 1e-6); }));
    }
    {
        Material mir;
        mir.is_reflective = true;
        std::vector<Material*> rm = {&mir};
        auto m2 = std::make_shared<PatchMesh>(verts, tris, rm);
        ObjectList w2;
        w2.add(m2);
        w2.build();
        TASK_CHECK(throws_invalid_argument([&] { PatchTransport t({m2}, w2, 1e-6); }));
    }
    return 0;
}

// ------------------------------------------------------------- inverse group

// Shared two-face scene (gap 0.2) with strong multi-bounce coupling.
struct TwoFaceScene {
    Material matA, matB;
    shared_ptr<PatchMesh> mesh;
    ObjectList world;
    TwoFaceScene() {
        matA.kd = 0.9;
        matA.diffusecolor = Radiance(0.95, 0.90, 0.85);
        matB.kd = 0.9;
        matB.diffusecolor = Radiance(0.90, 0.90, 0.90);
        std::vector<Point3> verts = {
            Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),
            Point3(0, 0, 0.2), Point3(0, 1, 0.2), Point3(1, 0, 0.2)};
        std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}};
        std::vector<Material*> mats = {&matA, &matB};
        mesh = std::make_shared<PatchMesh>(verts, tris, mats);
        world.add(mesh);
        world.build();
    }
};

// The fitted exitance solves the full multi-reflection equation, which a
// one-bounce fit visibly fails to do in this strongly coupled scene.
int test_multi_bounce_exact() {
    TwoFaceScene scene;
    PatchTransport transport({scene.mesh}, scene.world, 1e-6);

    std::vector<int> candidates = {0, 1};
    std::vector<FitObservation> observations = {
        {0, Radiance(0.8, 0.5, 0.3), 1.0},
        {1, Radiance(0.4, 0.6, 0.2), 1.0},
    };
    auto fit = transport.fit(candidates, observations, 1e-3, 1000);
    TASK_CHECK(fit.has_value());

    const int n = transport.face_count();
    // B satisfies B = E + rho F B (all reflection orders) exactly.
    double residual = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            double rhs = fit->emission(i)[c];
            for (int j = 0; j < n; ++j)
                rhs += transport.reflectance(i)[c] * transport.form_factor(i, j) *
                       fit->exitance(j)[c];
            residual = std::max(residual, std::fabs(fit->exitance(i)[c] - rhs));
        }
    }
    TASK_CHECK(residual < 1e-9);

    // The scene really exercises multiple bounces: the one-bounce
    // approximation E + rho F E is materially different from B.
    double one_bounce_diff = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            double b1 = fit->emission(i)[c];
            for (int j = 0; j < n; ++j)
                b1 += transport.reflectance(i)[c] * transport.form_factor(i, j) *
                      fit->emission(j)[c];
            one_bounce_diff = std::max(one_bounce_diff, std::fabs(fit->exitance(i)[c] - b1));
        }
    }
    TASK_CHECK(one_bounce_diff > 1e-3);

    // B matches the test's own independent solve of (I - rho F) B = E.
    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        for (int i = 0; i < n; ++i) {
            double b = 0.0;
            for (int j = 0; j < n; ++j) b += resp[i * n + j] * fit->emission(j)[c];
            TASK_CHECK(std::fabs(b - fit->exitance(i)[c]) < 1e-9);
        }
    }

    // Reported projected-gradient norms meet the per-channel criterion.
    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        double g0 = 0.0;
        for (int k = 0; k < 2; ++k) {
            double ck = 0.0;
            for (const auto& o : observations)
                ck += 2.0 * o.weight * resp[o.face * n + candidates[k]] * o.target[c];
            g0 = std::max(g0, std::fabs(ck));
        }
        const double tol = 1e-8 * std::max(1.0, g0);
        TASK_CHECK(fit->projected_gradient_norm(c) <= tol);
    }

    // Reported objective matches the test's own evaluation at the solution.
    double obj = 0.0;
    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        for (const auto& o : observations) {
            double b = 0.0;
            for (int k = 0; k < 2; ++k)
                b += resp[o.face * n + candidates[k]] * fit->emission(candidates[k])[c];
            obj += o.weight * (b - o.target[c]) * (b - o.target[c]);
        }
        for (int k = 0; k < 2; ++k)
            obj += 1e-3 * fit->emission(candidates[k])[c] * fit->emission(candidates[k])[c];
    }
    TASK_CHECK(std::fabs(obj - fit->objective()) < 1e-8);
    return 0;
}

// A three-face scene fitted against three observations (one with a negative
// target); the optimum is verified by enumerating all active sets.
int test_kkt_enumeration() {
    Material matA, matB, matC;
    matA.kd = 0.9; matA.diffusecolor = Radiance(0.95, 0.90, 0.85);
    matB.kd = 0.9; matB.diffusecolor = Radiance(0.90, 0.90, 0.90);
    matC.kd = 0.9; matC.diffusecolor = Radiance(0.80, 0.80, 0.80);

    std::vector<Point3> verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),          // 0: +z
        Point3(0, 0, 0.2), Point3(0, 1, 0.2), Point3(1, 0, 0.2),    // 1: -z
        Point3(2, 0, 0), Point3(2, 0, 1), Point3(2, 1, 0)};         // 2: -x wall
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}, {{6, 7, 8}}};
    std::vector<Material*> mats = {&matA, &matB, &matC};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);
    ObjectList world;
    world.add(mesh);
    world.build();
    PatchTransport transport({mesh}, world, 1e-6);

    std::vector<int> candidates = {0, 1};
    std::vector<FitObservation> observations = {
        {0, Radiance(0.7, 0.5, 0.3), 1.0},
        {1, Radiance(0.4, 0.6, 0.35), 1.0},
        {2, Radiance(0.2, -0.1, 0.25), 0.8}, // negative target component
    };
    const double lambda = 0.01;
    auto fit = transport.fit(candidates, observations, lambda, 1000);
    TASK_CHECK(fit.has_value());

    const int m = 2;
    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        // G, w, t for the independent enumeration.
        std::vector<double> G(3 * m), w(3), t(3);
        for (int o = 0; o < 3; ++o) {
            w[o] = observations[o].weight;
            t[o] = observations[o].target[c];
            for (int k = 0; k < m; ++k)
                G[o * m + k] = resp[observations[o].face * 3 + candidates[k]];
        }
        KktResult ref = kkt_enumerate(G, 3, m, w, t, lambda);
        TASK_CHECK(ref.found);
        for (int k = 0; k < m; ++k)
            TASK_CHECK(std::fabs(fit->emission(candidates[k])[c] - ref.x[k]) < 1e-8);
    }
    // Objective matches the independently enumerated optimum (summed over
    // the three channels).
    double ref_obj = 0.0;
    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        std::vector<double> G(3 * m), w(3), t(3);
        for (int o = 0; o < 3; ++o) {
            w[o] = observations[o].weight;
            t[o] = observations[o].target[c];
            for (int k = 0; k < m; ++k)
                G[o * m + k] = resp[observations[o].face * 3 + candidates[k]];
        }
        KktResult ref = kkt_enumerate(G, 3, m, w, t, lambda);
        ref_obj += ref.objective;
    }
    TASK_CHECK(std::fabs(fit->objective() - ref_obj) < 1e-8);
    return 0;
}

// Two symmetric, strongly correlated candidates: the data cannot tell them
// apart, the regularizer splits emission evenly, and the rank-deficient
// normal equations still solve.
int test_correlated_candidates() {
    Material matC, matF;
    matC.kd = 0.9; matC.diffusecolor = Radiance(0.9, 0.9, 0.9);
    matF.kd = 0.9; matF.diffusecolor = Radiance(0.8, 0.8, 0.8);

    std::vector<Point3> ceil_v = {
        Point3(-1, 1, 0), Point3(0, 1, 0), Point3(-0.5, 1, 0.8),  // L, -y
        Point3(0, 1, 0), Point3(1, 1, 0), Point3(0.5, 1, 0.8)};   // R, -y
    std::vector<std::array<int, 3>> ceil_t = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> ceil_m = {&matC, &matC};
    auto ceil = std::make_shared<PatchMesh>(ceil_v, ceil_t, ceil_m);

    std::vector<Point3> floor_v = {
        Point3(-0.4, -1, 0.3), Point3(0.4, -1, 0.3), Point3(0, -1, -0.5)};
    std::vector<std::array<int, 3>> floor_t = {{{0, 1, 2}}};
    std::vector<Material*> floor_m = {&matF};
    auto floor = std::make_shared<PatchMesh>(floor_v, floor_t, floor_m);

    ObjectList world;
    world.add(ceil);
    world.add(floor);
    world.build();
    PatchTransport transport({ceil, floor}, world, 1e-6);
    TASK_CHECK(transport.face_count() == 3);

    std::vector<int> candidates = {0, 1};
    std::vector<FitObservation> observations = {{2, Radiance(0.6, 0.5, 0.4), 1.0}};
    const double lambda = 0.001;
    auto fit = transport.fit(candidates, observations, lambda, 1000);
    TASK_CHECK(fit.has_value());

    // Symmetry: the two correlated candidates share the emission.
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(std::fabs(fit->emission(0)[c] - fit->emission(1)[c]) < 1e-6);

    // And the split is the KKT optimum.
    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        std::vector<double> G(2), w(1), t(1);
        G[0] = resp[2 * 3 + 0];
        G[1] = resp[2 * 3 + 1];
        w[0] = 1.0;
        t[0] = observations[0].target[c];
        KktResult ref = kkt_enumerate(G, 1, 2, w, t, lambda);
        TASK_CHECK(ref.found);
        TASK_CHECK(std::fabs(fit->emission(0)[c] - ref.x[0]) < 1e-8);
        TASK_CHECK(std::fabs(fit->emission(1)[c] - ref.x[1]) < 1e-8);
    }
    return 0;
}

// Negative targets are used as-is (never clamped to zero) and repeated
// observations accumulate as independent terms; an isolated face makes the
// optimum analytic.
int test_negative_targets_duplicates() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);
    std::vector<Point3> verts = {Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0)};
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}};
    std::vector<Material*> mats = {&mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);
    ObjectList world;
    world.add(mesh);
    world.build();
    PatchTransport transport({mesh}, world, 1e-6);

    const double lambda = 0.01;
    std::vector<FitObservation> observations = {
        {0, Radiance(-1.0, 2.0, 0.5), 1.0},
        {0, Radiance(3.0, 2.0, 0.5), 1.0},
        {0, Radiance(3.0, 2.0, 0.5), 1.0}, // duplicate: independent term
    };
    auto fit = transport.fit({0}, observations, lambda, 100);
    TASK_CHECK(fit.has_value());

    // Analytic optimum: x_c = sum(w t) / (sum(w) + lambda) per channel.
    const double er = 5.0 / 3.01, eg = 6.0 / 3.01, eb = 1.5 / 3.01;
    TASK_CHECK(std::fabs(fit->emission(0).x() - er) < 1e-9);
    TASK_CHECK(std::fabs(fit->emission(0).y() - eg) < 1e-9);
    TASK_CHECK(std::fabs(fit->emission(0).z() - eb) < 1e-9);
    // Isolated face: B == E.
    TASK_CHECK(fit->exitance(0).x() == fit->emission(0).x());
    // Clamping the negative target to zero first would give 6/3.01 in R.
    TASK_CHECK(std::fabs(fit->emission(0).x() - 6.0 / 3.01) > 0.1);

    double obj = 0.0;
    const double t[3][3] = {{-1.0, 2.0, 0.5}, {3.0, 2.0, 0.5}, {3.0, 2.0, 0.5}};
    const double x[3] = {er, eg, eb};
    for (int c = 0; c < 3; ++c) {
        for (int o = 0; o < 3; ++o) obj += (x[c] - t[o][c]) * (x[c] - t[o][c]);
        obj += lambda * x[c] * x[c];
    }
    TASK_CHECK(std::fabs(fit->objective() - obj) < 1e-9);

    // A purely negative target leaves the optimum on the zero boundary.
    auto fit2 = transport.fit({0}, {{0, Radiance(-2.0, -2.0, -2.0), 1.0}}, lambda, 100);
    TASK_CHECK(fit2.has_value());
    TASK_CHECK(fit2->emission(0).x() == 0.0);
    TASK_CHECK(fit2->emission(0).y() == 0.0);
    TASK_CHECK(fit2->emission(0).z() == 0.0);
    TASK_CHECK(std::fabs(fit2->objective() - 12.0) < 1e-9);
    return 0;
}

// A candidate fully occluded from the observed face has a zero response
// column; the rank-deficient problem still solves with x = 0 there.
int test_occluded_candidate() {
    Material mat;
    mat.kd = 0.9;
    mat.diffusecolor = Radiance(0.5, 0.5, 0.5);
    std::vector<Point3> verts = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),       // 0: +z
        Point3(0, 0, 1), Point3(0, 1, 1), Point3(1, 0, 1)};      // 1: -z
    std::vector<std::array<int, 3>> tris = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> mats = {&mat, &mat};
    auto mesh = std::make_shared<PatchMesh>(verts, tris, mats);
    // Sphere enclosing every sample point of face 0: fully occluded.
    auto blocker = std::make_shared<Sphere>(Point3(0.4, 0.4, 0.0), 0.4, &mat);
    ObjectList world;
    world.add(mesh);
    world.add(blocker);
    world.build();
    PatchTransport transport({mesh}, world, 1e-6);
    TASK_CHECK(transport.form_factor(0, 1) == 0.0);
    TASK_CHECK(transport.form_factor(1, 0) == 0.0);

    auto fit = transport.fit({0, 1}, {{1, Radiance(1.0, 1.0, 1.0), 1.0}}, 0.01, 1000);
    TASK_CHECK(fit.has_value());
    // The occluded candidate stays exactly at zero.
    TASK_CHECK(fit->emission(0).x() == 0.0);
    TASK_CHECK(fit->emission(0).y() == 0.0);
    TASK_CHECK(fit->emission(0).z() == 0.0);
    // The visible candidate carries the whole fit: x = 1 / (1 + lambda).
    TASK_CHECK(std::fabs(fit->emission(1).x() - 1.0 / 1.01) < 1e-9);
    TASK_CHECK(fit->exitance(1).x() == fit->emission(1).x());
    return 0;
}

// The unconstrained optimum goes negative on one candidate; the fit finds
// the true bound optimum instead of clamping the unconstrained solution.
int test_no_unconstrained_clamp() {
    Material matC, matO;
    matC.kd = 0.9; matC.diffusecolor = Radiance(0.9, 0.9, 0.9);
    matO.kd = 0.9; matO.diffusecolor = Radiance(0.8, 0.8, 0.8);

    std::vector<Point3> ceil_v = {
        Point3(-2, 1, 0), Point3(-1, 1, 0), Point3(-1.5, 1, 0.7),  // C1, -y
        Point3(1, 1, 0), Point3(2, 1, 0), Point3(1.5, 1, 0.7)};    // C2, -y
    std::vector<std::array<int, 3>> ceil_t = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> ceil_m = {&matC, &matC};
    auto ceil = std::make_shared<PatchMesh>(ceil_v, ceil_t, ceil_m);

    std::vector<Point3> obs_v = {
        Point3(-2, -1, 0.1), Point3(-1, -1, 0.1), Point3(-1.5, -1, -0.6), // O1, +y
        Point3(1, -1, 0.1), Point3(2, -1, 0.1), Point3(1.5, -1, -0.6)};   // O2, +y
    std::vector<std::array<int, 3>> obs_t = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> obs_m = {&matO, &matO};
    auto obs_mesh = std::make_shared<PatchMesh>(obs_v, obs_t, obs_m);

    ObjectList world;
    world.add(ceil);
    world.add(obs_mesh);
    world.build();
    PatchTransport transport({ceil, obs_mesh}, world, 1e-6);

    std::vector<int> candidates = {0, 1};
    std::vector<FitObservation> observations = {
        {2, Radiance(1.0, 0.8, 0.6), 1.0},
        {3, Radiance(-1.0, -0.8, -0.6), 1.0},
    };
    const double lambda = 0.01;
    auto fit = transport.fit(candidates, observations, lambda, 1000);
    TASK_CHECK(fit.has_value());

    for (int c = 0; c < 3; ++c) {
        std::vector<double> resp = reference_response(transport, c);
        std::vector<double> G(4), w(2), t(2);
        for (int o = 0; o < 2; ++o) {
            w[o] = 1.0;
            t[o] = observations[o].target[c];
            for (int k = 0; k < 2; ++k)
                G[o * 2 + k] = resp[observations[o].face * 4 + candidates[k]];
        }
        // Normal equations for this channel.
        std::vector<double> H(4, 0.0), cv(2, 0.0);
        for (int o = 0; o < 2; ++o)
            for (int k = 0; k < 2; ++k) {
                cv[k] += 2.0 * G[o * 2 + k] * t[o];
                for (int l = 0; l < 2; ++l) H[k * 2 + l] += 2.0 * G[o * 2 + k] * G[o * 2 + l];
            }
        H[0] += 2.0 * lambda;
        H[3] += 2.0 * lambda;
        // The unconstrained optimum really does go negative.
        std::vector<double> uc;
        TASK_CHECK(dense_solve(H, 2, cv, uc));
        TASK_CHECK(uc[1] < 0.0);
        // The fit puts candidate 2 exactly on the bound and candidate 1 at
        // the scalar KKT solution c1 / H11.
        TASK_CHECK(fit->emission(1)[c] == 0.0);
        const double x1 = cv[0] / H[0];
        TASK_CHECK(std::fabs(fit->emission(0)[c] - x1) < 1e-9);
        // Clamping the unconstrained solution is not this optimum.
        const double clamped = std::max(0.0, uc[0]);
        TASK_CHECK(std::fabs(clamped - x1) > 1e-9);
        double obj_clamped = 0.0;
        for (int o = 0; o < 2; ++o) {
            const double r = G[o * 2 + 0] * clamped - t[o];
            obj_clamped += r * r;
        }
        obj_clamped += lambda * clamped * clamped;
        double obj_fit_c = 0.0;
        for (int o = 0; o < 2; ++o) {
            const double r = G[o * 2 + 0] * x1 - t[o];
            obj_fit_c += r * r;
        }
        obj_fit_c += lambda * x1 * x1;
        TASK_CHECK(obj_clamped > obj_fit_c + 1e-9);
        // Independent active-set enumeration agrees.
        KktResult ref = kkt_enumerate(G, 2, 2, w, t, lambda);
        TASK_CHECK(ref.found);
        TASK_CHECK(std::fabs(ref.x[0] - x1) < 1e-9);
        TASK_CHECK(ref.x[1] == 0.0);
    }
    return 0;
}

// Invalid inputs and exhausted budgets fail; previously returned results
// stay untouched and no state leaks between fits.
int test_invalid_inputs_and_state() {
    TwoFaceScene scene;
    PatchTransport transport({scene.mesh}, scene.world, 1e-6);
    const double nan = std::numeric_limits<double>::quiet_NaN();

    std::vector<int> cand01 = {0, 1};
    std::vector<FitObservation> obs2 = {
        {0, Radiance(0.8, 0.6, 0.4), 1.0},
        {1, Radiance(0.5, 0.7, 0.3), 1.0},
    };
    auto r1 = transport.fit(cand01, obs2, 0.01, 1000);
    TASK_CHECK(r1.has_value());
    const Radiance e10 = r1->emission(0), e11 = r1->emission(1);
    const Radiance b10 = r1->exitance(0), b11 = r1->exitance(1);
    const double obj1 = r1->objective();

    // Every invalid call fails.
    TASK_CHECK(!transport.fit({}, obs2, 0.01, 1000));
    TASK_CHECK(!transport.fit(std::vector<int>(17, 0), obs2, 0.01, 1000));
    TASK_CHECK(!transport.fit({0, 0}, obs2, 0.01, 1000));
    TASK_CHECK(!transport.fit({-1}, obs2, 0.01, 1000));
    TASK_CHECK(!transport.fit({2}, obs2, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01,
                              std::vector<FitObservation>(257, {0, Radiance(1, 1, 1), 1.0}),
                              0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {{-1, Radiance(1, 1, 1), 1.0}}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {{2, Radiance(1, 1, 1), 1.0}}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {{0, Radiance(1, 1, 1), 0.0}}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {{0, Radiance(1, 1, 1), -1.0}}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {{0, Radiance(1, 1, 1), nan}}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, {{0, Radiance(nan, 1, 1), 1.0}}, 0.01, 1000));
    TASK_CHECK(!transport.fit(cand01, obs2, 0.0, 1000));
    TASK_CHECK(!transport.fit(cand01, obs2, -1.0, 1000));
    TASK_CHECK(!transport.fit(cand01, obs2, nan, 1000));
    TASK_CHECK(!transport.fit(cand01, obs2, 0.01, 0));
    TASK_CHECK(!transport.fit(cand01, obs2, 0.01, -5));

    // A positive but insufficient iteration budget fails too.
    std::vector<FitObservation> obs_sym = {
        {0, Radiance(1.0, 1.0, 1.0), 1.0},
        {1, Radiance(1.0, 1.0, 1.0), 1.0},
    };
    TASK_CHECK(!transport.fit(cand01, obs_sym, 0.01, 1));
    TASK_CHECK(transport.fit(cand01, obs_sym, 0.01, 1000).has_value());

    // The earlier result is item-wise unchanged after all the failures.
    TASK_CHECK(r1->emission(0).x() == e10.x() && r1->emission(0).y() == e10.y() &&
               r1->emission(0).z() == e10.z());
    TASK_CHECK(r1->emission(1).x() == e11.x() && r1->emission(1).y() == e11.y() &&
               r1->emission(1).z() == e11.z());
    TASK_CHECK(r1->exitance(0).x() == b10.x() && r1->exitance(1).x() == b11.x());
    TASK_CHECK(r1->objective() == obj1);

    // Re-running the same fit is deterministic (no leftover state).
    auto r1b = transport.fit(cand01, obs2, 0.01, 1000);
    TASK_CHECK(r1b.has_value());
    TASK_CHECK(std::fabs(r1b->objective() - obj1) < 1e-12);
    TASK_CHECK(std::fabs(r1b->emission(0).x() - e10.x()) < 1e-12);
    TASK_CHECK(std::fabs(r1b->emission(1).z() - e11.z()) < 1e-12);

    // A different fit keeps no residue of the previous candidates,
    // observations or emissions; the old result is still intact.
    auto r2 = transport.fit({0}, {{0, Radiance(0.3, 0.3, 0.3), 1.0}}, 0.01, 1000);
    TASK_CHECK(r2.has_value());
    TASK_CHECK(r2->emission(1).x() == 0.0 && r2->emission(1).y() == 0.0 &&
               r2->emission(1).z() == 0.0);
    TASK_CHECK(r2->emission(0).x() != e10.x());
    TASK_CHECK(r1->emission(0).x() == e10.x());
    TASK_CHECK(r1->objective() == obj1);
    return 0;
}

// -------------------------------------------------------------- render group

struct RenderScene {
    Material mats[7]; // panel, ceil, floor, back, stray, sphere, tri
    shared_ptr<PatchMesh> panels, ceil, floor, back, stray;
    shared_ptr<Sphere> sphere;
    shared_ptr<Triangle> tri;
    ObjectList world;
    // global face ids in transport order {panels, ceil, floor, back}
    static constexpr int kP0 = 0, kP1 = 1, kCeil = 2, kFloor = 3, kBack = 4;
};

void build_render_scene(RenderScene& s) {
    s.mats[0].kd = 0.9; s.mats[0].diffusecolor = Radiance(0.80, 0.80, 0.80);
    s.mats[1].kd = 0.9; s.mats[1].diffusecolor = Radiance(0.85, 0.85, 0.85);
    s.mats[2].kd = 0.9; s.mats[2].diffusecolor = Radiance(0.70, 0.70, 0.70);
    s.mats[3].kd = 0.9; s.mats[3].diffusecolor = Radiance(0.80, 0.80, 0.80);
    s.mats[4].kd = 0.9; s.mats[4].diffusecolor = Radiance(0.80, 0.80, 0.80);
    s.mats[5].kd = 0.9; s.mats[5].diffusecolor = Radiance(0.60, 0.60, 0.60);
    s.mats[6].kd = 0.9; s.mats[6].diffusecolor = Radiance(0.60, 0.60, 0.60);

    // Two panels at z = -2 facing the camera (+z).
    std::vector<Point3> pv = {
        Point3(-2.5, -1.5, -2), Point3(-0.1, -1.5, -2), Point3(-1.3, 1.8, -2),
        Point3(0.1, -1.5, -2), Point3(2.5, -1.5, -2), Point3(1.3, 1.8, -2)};
    std::vector<std::array<int, 3>> pt = {{{0, 1, 2}}, {{3, 4, 5}}};
    std::vector<Material*> pm = {&s.mats[0], &s.mats[0]};
    s.panels = std::make_shared<PatchMesh>(pv, pt, pm);

    // Ceiling at y = 2 facing down (-y); the fitted emitter.
    std::vector<Point3> cv = {Point3(2.5, 2, 1), Point3(-2.5, 2, 1), Point3(0, 2, -3)};
    std::vector<std::array<int, 3>> ct = {{{0, 1, 2}}};
    std::vector<Material*> cm = {&s.mats[1]};
    s.ceil = std::make_shared<PatchMesh>(cv, ct, cm);

    // Floor at y = -2 facing up (+y); lit only by bounced light.
    std::vector<Point3> fv = {Point3(-2.5, -2, 1), Point3(2.5, -2, 1), Point3(0, -2, -3)};
    std::vector<std::array<int, 3>> ft = {{{0, 1, 2}}};
    std::vector<Material*> fm = {&s.mats[2]};
    s.floor = std::make_shared<PatchMesh>(fv, ft, fm);

    // Triangle at z = -1 facing away from the camera (normal -z).
    std::vector<Point3> bv = {
        Point3(-1.2, 1.2, -1), Point3(-2.2, 1.2, -1), Point3(-1.7, 2.0, -1)};
    std::vector<std::array<int, 3>> bt = {{{0, 1, 2}}};
    std::vector<Material*> bm = {&s.mats[3]};
    s.back = std::make_shared<PatchMesh>(bv, bt, bm);

    // Patch mesh in the world but not in the fit (normal +z).
    std::vector<Point3> sv = {
        Point3(1.5, 1.3, -1), Point3(2.3, 1.3, -1), Point3(1.9, 2.0, -1)};
    std::vector<std::array<int, 3>> st = {{{0, 1, 2}}};
    std::vector<Material*> sm = {&s.mats[4]};
    s.stray = std::make_shared<PatchMesh>(sv, st, sm);

    // Occluders: a sphere in front of panel 1 and a regular Triangle.
    s.sphere = std::make_shared<Sphere>(Point3(0.9, 0.2, -0.5), 0.35, &s.mats[5]);
    s.tri = std::make_shared<Triangle>(Point3(-2.0, -1.6, -1), Point3(-1.2, -1.6, -1),
                                       Point3(-1.6, -0.9, -1), &s.mats[6]);

    s.world.add(s.panels);
    s.world.add(s.ceil);
    s.world.add(s.floor);
    s.world.add(s.back);
    s.world.add(s.stray);
    s.world.add(s.sphere);
    s.world.add(s.tri);
    s.world.build();
}

Camera make_test_camera(const std::string& filename) {
    Camera cam;
    cam.image_width = 64;
    cam.image_height = 64;
    cam.lookfrom = Point3(0, 0, 5);
    cam.lookat = Point3(0, 0, 0);
    cam.vup = Vector3(0, 1, 0);
    cam.vfov = 90;
    cam.aperture = 0.0;
    cam.samples_per_pixel = 1;
    cam.sampling_method = AntiAliasing::RANDOM;
    cam.tone_mapper = ToneMapper::LINEAR;
    cam.exposure = 0.0; // tone_exposure = 1
    cam.background = Radiance(0.05, 0.10, 0.15);
    cam.filename = filename;
    return cam;
}

// Probe pixels (framebuffer indices) for the test camera:
// px = x * 5 / (5 - z), i = (px + 5) * 6.4; py likewise for j.
bool probe_matches(const std::vector<int>& values, int w, int i, int j,
                   const Camera& cam, const Radiance& expected_hdr, int tol) {
    const std::array<int, 3> px = ppm_pixel(values, w, i, j);
    bool ok = true;
    for (int c = 0; c < 3; ++c) {
        const int want = tone_byte(cam, expected_hdr, c);
        ok = ok && std::fabs(px[c] - want) <= tol;
    }
    return ok;
}

// Render a fitted result with the original camera: source-face attribution
// and B/pi on front faces, black for back faces / occluders / unknown
// meshes / regular primitives, camera background elsewhere.
int test_render_fitted_scene() {
    RenderScene scene;
    build_render_scene(scene);
    Radiance before[7];
    for (int k = 0; k < 7; ++k) before[k] = scene.mats[k].diffusecolor;

    PatchTransport transport({scene.panels, scene.ceil, scene.floor, scene.back},
                             scene.world, 1e-6);
    std::vector<FitObservation> observations = {
        {RenderScene::kP0, Radiance(0.85, 0.65, 0.45), 1.0},
        {RenderScene::kP1, Radiance(0.55, 0.75, 0.60), 1.0},
    };
    auto fit = transport.fit({RenderScene::kCeil}, observations, 1e-4, 1000);
    TASK_CHECK(fit.has_value());

    // The floor emits nothing yet is lit by bounced light; the back-facing
    // triangle is not a candidate either.
    TASK_CHECK(fit->emission(RenderScene::kFloor).x() == 0.0);
    TASK_CHECK(fit->exitance(RenderScene::kFloor).x() > 0.01);
    TASK_CHECK(fit->emission(RenderScene::kBack).x() == 0.0);

    Camera cam = make_test_camera(".task-build/render_scene_a.ppm");
    cam.render_fitted(scene.world, *fit);

    int w = 0, h = 0;
    std::vector<int> values;
    TASK_CHECK(read_ppm(".task-build/render_scene_a.ppm", w, h, values));
    TASK_CHECK(w == 64 && h == 64);
    TASK_CHECK(values.size() == 64 * 64 * 3);

    // Source-face attribution and B/pi on the four fitted faces.
    TASK_CHECK(probe_matches(values, w, 26, 32, cam, fit->exitance(RenderScene::kP0) / pi, 1));
    TASK_CHECK(probe_matches(values, w, 40, 32, cam, fit->exitance(RenderScene::kP1) / pi, 1));
    TASK_CHECK(probe_matches(values, w, 32, 21, cam, fit->exitance(RenderScene::kCeil) / pi, 1));
    TASK_CHECK(probe_matches(values, w, 32, 42, cam, fit->exitance(RenderScene::kFloor) / pi, 1));

    // The two panels carry distinct values (attribution is sensitive).
    TASK_CHECK(std::fabs(fit->exitance(RenderScene::kP0).x() -
                         fit->exitance(RenderScene::kP1).x()) > 0.05);

    // Occluders, back faces, unknown meshes and regular primitives: black.
    TASK_CHECK(probe_matches(values, w, 37, 30, cam, Radiance(0, 0, 0), 0)); // sphere
    TASK_CHECK(probe_matches(values, w, 22, 24, cam, Radiance(0, 0, 0), 0)); // back face
    TASK_CHECK(probe_matches(values, w, 42, 24, cam, Radiance(0, 0, 0), 0)); // stray mesh
    TASK_CHECK(probe_matches(values, w, 23, 39, cam, Radiance(0, 0, 0), 0)); // Triangle

    // Background pixels use the camera background.
    TASK_CHECK(probe_matches(values, w, 2, 2, cam, cam.background, 0));
    TASK_CHECK(probe_matches(values, w, 61, 61, cam, cam.background, 0));

    // Shared materials were never rewritten to fake the fitted output.
    for (int k = 0; k < 7; ++k) {
        TASK_CHECK(scene.mats[k].diffusecolor.x() == before[k].x());
        TASK_CHECK(scene.mats[k].diffusecolor.y() == before[k].y());
        TASK_CHECK(scene.mats[k].diffusecolor.z() == before[k].z());
    }
    return 0;
}

// Two fit results render interleaved without interfering, and results stay
// readable and drawable after their transport object is destroyed.
int test_render_results_isolated() {
    RenderScene scene;
    build_render_scene(scene);
    Radiance before[7];
    for (int k = 0; k < 7; ++k) before[k] = scene.mats[k].diffusecolor;

    std::optional<EmissionFit> fit_a, fit_b;
    {
        PatchTransport transport({scene.panels, scene.ceil, scene.floor, scene.back},
                                 scene.world, 1e-6);
        fit_a = transport.fit({RenderScene::kCeil},
                              {{RenderScene::kP0, Radiance(0.85, 0.65, 0.45), 1.0}},
                              1e-4, 1000);
        TASK_CHECK(fit_a.has_value());
    } // transport destroyed here
    {
        PatchTransport transport({scene.panels, scene.ceil, scene.floor, scene.back},
                                 scene.world, 1e-6);
        fit_b = transport.fit({RenderScene::kCeil},
                              {{RenderScene::kP0, Radiance(0.30, 0.50, 0.70), 1.0}},
                              1e-4, 1000);
        TASK_CHECK(fit_b.has_value());
    }

    // The result still reads fine after its transport is gone.
    TASK_CHECK(fit_a->exitance(RenderScene::kP0).x() > 0.5);
    Radiance e, b;
    TASK_CHECK(fit_a->lookup(scene.panels.get(), 0, e, b));
    TASK_CHECK(b.x() == fit_a->exitance(RenderScene::kP0).x());
    TASK_CHECK(!fit_a->lookup(scene.stray.get(), 0, e, b));

    // The two fits really differ on the observed panel.
    TASK_CHECK(std::fabs(fit_a->exitance(RenderScene::kP0).x() -
                         fit_b->exitance(RenderScene::kP0).x()) > 0.2);

    // Interleaved rendering: A, then B, then A again.
    Camera cam_a = make_test_camera(".task-build/render_iso_a.ppm");
    cam_a.render_fitted(scene.world, *fit_a);
    Camera cam_b = make_test_camera(".task-build/render_iso_b.ppm");
    cam_b.render_fitted(scene.world, *fit_b);
    Camera cam_a2 = make_test_camera(".task-build/render_iso_a2.ppm");
    cam_a2.render_fitted(scene.world, *fit_a);

    int w = 0, h = 0;
    std::vector<int> va, vb, va2;
    TASK_CHECK(read_ppm(".task-build/render_iso_a.ppm", w, h, va));
    TASK_CHECK(read_ppm(".task-build/render_iso_b.ppm", w, h, vb));
    TASK_CHECK(read_ppm(".task-build/render_iso_a2.ppm", w, h, va2));

    // Each image shows its own result's B/pi on the panels.
    TASK_CHECK(probe_matches(va, w, 26, 32, cam_a, fit_a->exitance(RenderScene::kP0) / pi, 1));
    TASK_CHECK(probe_matches(va, w, 40, 32, cam_a, fit_a->exitance(RenderScene::kP1) / pi, 1));
    TASK_CHECK(probe_matches(vb, w, 26, 32, cam_b, fit_b->exitance(RenderScene::kP0) / pi, 1));
    TASK_CHECK(probe_matches(vb, w, 40, 32, cam_b, fit_b->exitance(RenderScene::kP1) / pi, 1));

    // Re-rendering A reproduces the same image at the probes.
    for (int probe = 0; probe < 2; ++probe) {
        const int i = probe == 0 ? 26 : 40;
        const std::array<int, 3> p1 = ppm_pixel(va, w, i, 32);
        const std::array<int, 3> p2 = ppm_pixel(va2, w, i, 32);
        TASK_CHECK(p1 == p2);
    }

    // The two results' images differ on the panels.
    const std::array<int, 3> pa = ppm_pixel(va, w, 26, 32);
    const std::array<int, 3> pb = ppm_pixel(vb, w, 26, 32);
    TASK_CHECK(std::fabs(pa[0] - pb[0]) > 3);

    // Materials still untouched.
    for (int k = 0; k < 7; ++k) {
        TASK_CHECK(scene.mats[k].diffusecolor.x() == before[k].x());
        TASK_CHECK(scene.mats[k].diffusecolor.y() == before[k].y());
        TASK_CHECK(scene.mats[k].diffusecolor.z() == before[k].z());
    }
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"geometry", "mesh_intersection_identity", test_mesh_intersection_identity},
    {"geometry", "source_face_stable", test_source_face_stable},
    {"geometry", "form_factor_reference", test_form_factor_reference},
    {"geometry", "form_factor_scaling", test_form_factor_scaling},
    {"geometry", "form_factor_areas", test_form_factor_areas},
    {"geometry", "backface_no_transfer", test_backface_no_transfer},
    {"geometry", "occlusion_variants", test_occlusion_variants},
    {"geometry", "eps_distance_rule", test_eps_distance_rule},
    {"geometry", "invalid_construction", test_invalid_construction},
    {"inverse", "multi_bounce_exact", test_multi_bounce_exact},
    {"inverse", "kkt_enumeration", test_kkt_enumeration},
    {"inverse", "correlated_candidates", test_correlated_candidates},
    {"inverse", "negative_targets_duplicates", test_negative_targets_duplicates},
    {"inverse", "occluded_candidate", test_occluded_candidate},
    {"inverse", "no_unconstrained_clamp", test_no_unconstrained_clamp},
    {"inverse", "invalid_inputs_and_state", test_invalid_inputs_and_state},
    {"render", "render_fitted_scene", test_render_fitted_scene},
    {"render", "render_results_isolated", test_render_results_isolated},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
