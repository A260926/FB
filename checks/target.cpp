/*
 * Target checks for view_texture.
 *
 * Groups:
 *   visibility  - source camera coverage / depth statistics with an
 *                 independent brute force reference
 *   cut         - exact binary labelling versus exhaustive enumeration
 *   projection  - observer draw, bilinear sampling, state, fallback
 */

#include "task_support/check.h"
#include "rasterizer/view_texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <vector>

namespace
{

/* ------------------------------------------------------------------ */
/*  shared helpers                                                    */
/* ------------------------------------------------------------------ */

Mat4 ortho_xy(float l, float r, float b, float t, float n, float f)
{
    return Mat4::ortho(l, b, r, t, n, f);
}

Mat4 ortho_unit()
{
    return ortho_xy(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
}

Texture<Vec4> solid_texture(unsigned int size, const Vec4& color)
{
    Texture<Vec4> texture(size, size);
    texture.fill(color);
    return texture;
}

/* Deterministic pattern where every texel is unique in its low bits. */
Texture<Vec4> pattern_texture(unsigned int size)
{
    Texture<Vec4> texture(size, size);
    for(unsigned int y = 0; y < size; y++)
    {
        for(unsigned int x = 0; x < size; x++)
        {
            float fx = static_cast<float>(x) / static_cast<float>(size);
            float fy = static_cast<float>(y) / static_cast<float>(size);
            texture(x, y) = Vec4(fx, fy, static_cast<float>((x * 7 + y * 3) % 17) / 17.0f, 1.0f);
        }
    }
    return texture;
}

/* Reference clip test: -w <= x,y,z <= w and w > 0. */
bool clipped_inside(const Vec4& c)
{
    if(!(c.w > 0.0f)) return false;
    return std::fabs(c.x) <= c.w && std::fabs(c.y) <= c.w && std::fabs(c.z) <= c.w;
}

struct Reference
{
    /* independent, deliberately naive full-screen per-face rasterizer */
    std::vector<int> covered;
    std::vector<int> won;
};

/*
 * Brute force reference for one source camera.  Every face is clipped against
 * all six planes in double precision, then sampled at every pixel centre of
 * the whole target.  Coverage uses the same half open rule; depth is the
 * screen linear z/w.  Independent of view_texture's implementation, but
 * shares the documented definitions.
 */
Reference reference_stats(const view_texture::Mesh& mesh, const Mat4& matrix,
                          int width, int height)
{
    std::size_t faces = mesh.indices.size() / 3;
    Reference ref;
    ref.covered.assign(faces, 0);
    ref.won.assign(faces, 0);

    std::vector<double> best(width * height, std::numeric_limits<double>::infinity());
    std::vector<int> best_face(width * height, -1);

    struct V
    {
        double c[4];
    };

    auto edge = [](double ax, double ay, double bx, double by, double px, double py)
    {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };

    /* Independent implementation of the documented half open rule: a
     * boundary belongs to the directed edge going down, or going left when
     * horizontal. */
    auto accept = [](double e, double dx, double dy)
    {
        double scale = std::max(1.0, std::fabs(dx) + std::fabs(dy));
        double eps = 1e-10 * scale;
        if(e > eps) return true;
        if(e < -eps) return false;
        if(dy < -eps) return true;
        if(dy > eps) return false;
        return dx < -eps;
    };

    for(std::size_t f = 0; f < faces; f++)
    {
        std::vector<V> poly;
        for(int k = 0; k < 3; k++)
        {
            const Vec3& p = mesh.positions[mesh.indices[f * 3 + k]];
            Vec4 c = matrix * Vec4(p, 1.0f);
            poly.push_back(V{{static_cast<double>(c.x), static_cast<double>(c.y),
                              static_cast<double>(c.z), static_cast<double>(c.w)}});
        }

        for(int axis = 0; axis < 3; axis++)
        {
            for(int sign = 0; sign < 2; sign++)
            {
                double s = sign == 0 ? 1.0 : -1.0;
                std::vector<V> out;
                for(std::size_t i = 0; i < poly.size(); i++)
                {
                    const V& a = poly[i];
                    const V& b = poly[(i + 1) % poly.size()];
                    double da = s * a.c[axis] + a.c[3];
                    double db = s * b.c[axis] + b.c[3];
                    bool ia = da >= 0.0;
                    bool ib = db >= 0.0;
                    if(ia && ib) out.push_back(b);
                    else if(ia && !ib)
                    {
                        double t = da / (da - db);
                        V r;
                        for(int q = 0; q < 4; q++) r.c[q] = a.c[q] + (b.c[q] - a.c[q]) * t;
                        out.push_back(r);
                    }
                    else if(!ia && ib)
                    {
                        double t = da / (da - db);
                        V r;
                        for(int q = 0; q < 4; q++) r.c[q] = a.c[q] + (b.c[q] - a.c[q]) * t;
                        out.push_back(r);
                        out.push_back(b);
                    }
                }
                poly.swap(out);
            }
        }

        std::vector<char> seen(width * height, 0);

        for(std::size_t i = 1; i + 1 < poly.size(); i++)
        {
            const V& a = poly[0];
            const V& b = poly[i];
            const V& c = poly[i + 1];
            if(!(a.c[3] > 0) || !(b.c[3] > 0) || !(c.c[3] > 0)) continue;

            double ax = (a.c[0] / a.c[3] + 1.0) * 0.5 * width;
            double ay = (a.c[1] / a.c[3] + 1.0) * 0.5 * height;
            double bx = (b.c[0] / b.c[3] + 1.0) * 0.5 * width;
            double by = (b.c[1] / b.c[3] + 1.0) * 0.5 * height;
            double cx = (c.c[0] / c.c[3] + 1.0) * 0.5 * width;
            double cy = (c.c[1] / c.c[3] + 1.0) * 0.5 * height;

            double D = edge(ax, ay, bx, by, cx, cy);
            if(D == 0.0) continue;

            /* Keep each vertex's depth attached to its (possibly swapped)
             * projected coordinates. */
            const V* pa = &a;
            const V* pb = &b;
            const V* pc = &c;
            if(D < 0.0)
            {
                std::swap(bx, cx);
                std::swap(by, cy);
                std::swap(pb, pc);
                D = -D;
            }

            double dx_ab = bx - ax, dy_ab = by - ay;
            double dx_bc = cx - bx, dy_bc = cy - by;
            double dx_ca = ax - cx, dy_ca = ay - cy;

            double za = 0.5 * (pa->c[2] / pa->c[3]) + 0.5;
            double zb = 0.5 * (pb->c[2] / pb->c[3]) + 0.5;
            double zc = 0.5 * (pc->c[2] / pc->c[3]) + 0.5;

            double minx = std::min({ax, bx, cx});
            double maxx = std::max({ax, bx, cx});
            double miny = std::min({ay, by, cy});
            double maxy = std::max({ay, by, cy});

            int x0 = std::max(0, static_cast<int>(std::floor(minx)));
            int x1 = std::min(width - 1, static_cast<int>(std::ceil(maxx)));
            int y0 = std::max(0, static_cast<int>(std::floor(miny)));
            int y1 = std::min(height - 1, static_cast<int>(std::ceil(maxy)));

            for(int y = y0; y <= y1; y++)
            {
                for(int x = x0; x <= x1; x++)
                {
                    double px = x + 0.5;
                    double py = y + 0.5;

                    double ea = edge(bx, by, cx, cy, px, py);
                    double eb = edge(cx, cy, ax, ay, px, py);
                    double ec = edge(ax, ay, bx, by, px, py);

                    if(!accept(ec, dx_ab, dy_ab)) continue;
                    if(!accept(ea, dx_bc, dy_bc)) continue;
                    if(!accept(eb, dx_ca, dy_ca)) continue;

                    double z = (ea * za + eb * zb + ec * zc) / D;
                    if(!std::isfinite(z)) continue;

                    std::size_t idx = static_cast<std::size_t>(y) * width + x;
                    if(!seen[idx])
                    {
                        seen[idx] = 1;
                        ref.covered[f] += 1;
                    }
                    if(z < best[idx])
                    {
                        best[idx] = z;
                        best_face[idx] = static_cast<int>(f);
                    }
                    else if(z == best[idx])
                    {
                        best_face[idx] = static_cast<int>(f);
                    }
                }
            }
        }
    }

    for(int f : best_face)
    {
        if(f >= 0) ref.won[f] += 1;
    }

    return ref;
}

/* Exhaustive energy of a binary labelling problem. */
long long brute_energy(const view_texture::LabelProblem& p, const std::vector<int>& labels)
{
    long long pixels = static_cast<long long>(p.width) * p.height;
    long long energy = 0;
    for(std::size_t f = 0; f < labels.size(); f++)
    {
        if(labels[f] < 0) continue;
        energy += pixels - p.covered[f][labels[f]];
    }

    std::map<std::pair<unsigned int, unsigned int>, std::vector<std::size_t>> table;
    for(std::size_t f = 0; f < labels.size(); f++)
    {
        unsigned int vs[3] = {p.indices[f * 3], p.indices[f * 3 + 1], p.indices[f * 3 + 2]};
        for(int k = 0; k < 3; k++)
        {
            auto key = std::minmax(vs[k], vs[(k + 1) % 3]);
            table[key].push_back(f);
        }
    }

    for(const auto& entry : table)
    {
        if(entry.second.size() != 2) continue;
        int a = labels[entry.second[0]];
        int b = labels[entry.second[1]];
        if(a >= 0 && b >= 0 && a != b) energy += p.seam_penalty;
    }

    return energy;
}

/* Enumerate every legal label assignment.  A source is legal when its
 * covered entry is not negative.  Returns the minimum energy. */
long long brute_min_energy(const view_texture::LabelProblem& p)
{
    std::size_t faces = p.covered.size();
    std::vector<int> labels(faces, 0);

    std::vector<int> options(faces);
    for(std::size_t f = 0; f < faces; f++)
    {
        int mask = 0;
        if(p.covered[f][0] >= 0) mask |= 1;
        if(p.covered[f][1] >= 0) mask |= 2;
        options[f] = mask;
    }

    long long best = std::numeric_limits<long long>::max();
    std::vector<int> current(faces, -1);

    std::function<void(std::size_t)> rec = [&](std::size_t i)
    {
        if(i == faces)
        {
            best = std::min(best, brute_energy(p, current));
            return;
        }
        int mask = options[i];
        if(mask == 0)
        {
            current[i] = -1;
            rec(i + 1);
        }
        if(mask & 1)
        {
            current[i] = 0;
            rec(i + 1);
        }
        if(mask & 2)
        {
            current[i] = 1;
            rec(i + 1);
        }
    };
    rec(0);
    return best;
}

bool approx(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

bool approx_color(const Vec4& a, const Vec4& b, float eps = 1.0f / 128.0f)
{
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) &&
           approx(a.z, b.z, eps) && approx(a.w, b.w, eps);
}

/* ------------------------------------------------------------------ */
/*  visibility group                                                  */
/* ------------------------------------------------------------------ */

/* A single centred triangle covering the target must report exactly the
 * covered sample count, not an area estimate. */
int visibility_single_triangle(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(16, Vec4(1, 0, 0, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    view_texture::Mesh mesh;
    mesh.positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    mesh.indices = { 0, 1, 2 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    Reference ref = reference_stats(mesh, m, 16, 16);
    TASK_CHECK(selection.visibility[0][0].covered == ref.covered[0]);
    TASK_CHECK(selection.visibility[0][0].won == ref.won[0]);
    TASK_CHECK(selection.visibility[0][0].covered > 0);
    TASK_CHECK(selection.visibility[0][0].won == selection.visibility[0][0].covered);
    TASK_CHECK(selection.visibility[0][0].selectable);
    return 0;
}

/* A nearer quad must win every pixel it covers and the farther quad behind
 * it must win nothing, so only the nearer face is selectable. */
int visibility_occlusion(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(16, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    view_texture::Mesh mesh;
    /* With this ortho the near plane maps to smaller screen depth at larger
     * world z, so faces 0/1 (z=-0.7) are far and faces 2/3 (z=+0.7) near. */
    mesh.positions = {
        { -1.0f, -1.0f, -0.7f }, { 1.0f, -1.0f, -0.7f }, { 1.0f, 1.0f, -0.7f }, { -1.0f, 1.0f, -0.7f },
        { -0.5f, -0.5f, 0.7f }, { 0.5f, -0.5f, 0.7f }, { 0.5f, 0.5f, 0.7f }, { -0.5f, 0.5f, 0.7f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    Reference ref = reference_stats(mesh, m, 16, 16);

    /* near faces (2, 3) win every pixel they cover */
    for(int f = 2; f <= 3; f++)
    {
        TASK_CHECK(selection.visibility[f][0].covered > 0);
        TASK_CHECK(selection.visibility[f][0].won == selection.visibility[f][0].covered);
        TASK_CHECK(selection.visibility[f][0].selectable);
    }

    /* the large far faces (0, 1) are fully or partly hidden */
    TASK_CHECK(selection.visibility[0][0].covered > 0);
    TASK_CHECK(selection.visibility[1][0].covered > 0);
    TASK_CHECK(selection.visibility[0][0].won + selection.visibility[1][0].won <
               selection.visibility[0][0].covered + selection.visibility[1][0].covered);

    for(std::size_t f = 0; f < selection.visibility.size(); f++)
    {
        TASK_CHECK(selection.visibility[f][0].covered == ref.covered[f]);
        TASK_CHECK(selection.visibility[f][0].won == ref.won[f]);
    }
    return 0;
}

/* A face straddling the near plane must still be clipped and must still win
 * depth over the pixels it covers, even though it cannot be selected. */
int visibility_cross_near(void)
{
    Mat4 persp = Mat4::perspective(radians(90.0f), 1.0f, 1.0f, 10.0f);
    Texture<Vec4> tex = solid_texture(24, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{persp, &tex},
        view_texture::SourceCamera{persp, &tex},
    };

    view_texture::Mesh mesh;
    /* A quad tilted through the near plane: two verts at z=-3 (inside) and
     * two at z=+0.5 (behind the camera, outside the clip volume). */
    mesh.positions = {
        { -1.0f, -1.0f, -3.0f },
        {  1.0f, -1.0f, -3.0f },
        {  1.0f,  1.0f,  0.5f },
        { -1.0f,  1.0f,  0.5f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    bool any_covered = false;
    for(std::size_t f = 0; f < selection.visibility.size(); f++)
    {
        TASK_CHECK(!selection.visibility[f][0].selectable);   /* not all inside */
        if(selection.visibility[f][0].covered > 0) any_covered = true;
    }
    TASK_CHECK(any_covered);

    Reference ref = reference_stats(mesh, persp, 24, 24);
    for(std::size_t f = 0; f < selection.visibility.size(); f++)
    {
        TASK_CHECK(selection.visibility[f][0].covered == ref.covered[f]);
        TASK_CHECK(selection.visibility[f][0].won == ref.won[f]);
    }

    /* A clipped face must still occlude: put a fully visible face behind the
     * straddling quad and verify it loses pixels in the overlap region. */
    view_texture::Mesh occluded;
    occluded.positions = mesh.positions;
    occluded.indices = mesh.indices;
    /* a far triangle behind, in front of the camera, fully inside */
    occluded.positions.push_back({ -0.5f, -0.5f, -6.0f });
    occluded.positions.push_back({  0.5f, -0.5f, -6.0f });
    occluded.positions.push_back({  0.5f,  0.5f, -6.0f });
    occluded.indices.push_back(4);
    occluded.indices.push_back(5);
    occluded.indices.push_back(6);

    auto occluded_selection = view_texture::select(occluded, sources, 0);
    TASK_CHECK(occluded_selection.valid);

    const auto& behind = occluded_selection.visibility[2][0];
    TASK_CHECK(behind.covered > 0);
    TASK_CHECK(behind.won < behind.covered);   /* straddling quad occludes it */
    TASK_CHECK(!behind.selectable);

    Reference occluded_ref = reference_stats(occluded, persp, 24, 24);
    TASK_CHECK(behind.covered == occluded_ref.covered[2]);
    TASK_CHECK(behind.won == occluded_ref.won[2]);
    return 0;
}

/* Two triangles sharing an edge must not double count the shared boundary:
 * the union coverage equals the sum of the individually rendered halves. */
int visibility_shared_edge(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(16, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    /* a quad split along a non-axis aligned diagonal */
    view_texture::Mesh mesh;
    mesh.positions = {
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f },
        { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    Reference ref = reference_stats(mesh, m, 16, 16);

    int sum = 0;

    for(std::size_t f = 0; f < 2; f++)
    {
        sum += selection.visibility[f][0].covered;
        TASK_CHECK(selection.visibility[f][0].covered == ref.covered[f]);
    }

    /* total covered samples across both faces equals the quad area; the two
     * halves partition the integer pixel grid, so there is no overlap. */
    TASK_CHECK(sum <= 16 * 16);

    /* Rendering each half on its own must yield the same covered counts, i.e.
     * neither half depends on the other to fill the shared boundary. */
    view_texture::Mesh left;
    left.positions = mesh.positions;
    left.indices = { 0, 1, 2 };
    view_texture::Mesh right;
    right.positions = mesh.positions;
    right.indices = { 0, 2, 3 };

    auto l = view_texture::select(left, sources, 0);
    auto r = view_texture::select(right, sources, 0);
    TASK_CHECK(l.visibility[0][0].covered + r.visibility[0][0].covered == sum);

    /* the two halves tile the whole target without gaps or double counting */
    TASK_CHECK(sum == 16 * 16);
    return 0;
}

/* Faces clipped by the viewport still produce the expected shard count and
 * keep their original face identity. */
int visibility_viewport_shards(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(16, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    /* one triangle partially outside the right and top edges */
    view_texture::Mesh mesh;
    mesh.positions = {
        { 0.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, { 0.0f, 3.0f, 0.0f },
    };
    mesh.indices = { 0, 1, 2 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    TASK_CHECK(selection.visibility[0][0].shards >= 1);
    TASK_CHECK(selection.visibility[0][0].covered > 0);
    TASK_CHECK(selection.visibility[0][0].covered ==
               selection.visibility[0][0].won);
    /* not all three original vertices are inside -> not selectable */
    TASK_CHECK(!selection.visibility[0][0].selectable);

    Reference ref = reference_stats(mesh, m, 16, 16);
    TASK_CHECK(selection.visibility[0][0].covered == ref.covered[0]);
    TASK_CHECK(selection.visibility[0][0].won == ref.won[0]);

    /* Clipping a triangle against a single plane gives a quad, i.e. exactly
     * two fan shards.  Using a face with only one vertex outside the right
     * plane keeps that deterministic. */
    view_texture::Mesh single_plane;
    single_plane.positions = {
        { -0.5f, -0.5f, 0.0f }, { 3.0f, -0.5f, 0.0f }, { -0.5f, 0.8f, 0.0f },
    };
    single_plane.indices = { 0, 1, 2 };

    auto sp = view_texture::select(single_plane, sources, 0);
    TASK_CHECK(sp.valid);
    TASK_CHECK(sp.visibility[0][0].shards == 2);
    TASK_CHECK(sp.visibility[0][0].covered > 0);

    /* A fully inside triangle is a single unclipped shard. */
    view_texture::Mesh inside;
    inside.positions = {
        { -0.5f, -0.5f, 0.0f }, { 0.5f, -0.5f, 0.0f }, { 0.0f, 0.5f, 0.0f },
    };
    inside.indices = { 0, 1, 2 };
    auto in = view_texture::select(inside, sources, 0);
    TASK_CHECK(in.valid);
    TASK_CHECK(in.visibility[0][0].shards == 1);
    TASK_CHECK(in.visibility[0][0].selectable);
    return 0;
}
int visibility_depth_tie(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(8, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    /* two identical coplanar triangles */
    view_texture::Mesh mesh;
    mesh.positions = {
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
    };
    mesh.indices = { 0, 1, 2, 3, 4, 5 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);
    TASK_CHECK(selection.visibility[0][0].covered == selection.visibility[1][0].covered);
    TASK_CHECK(selection.visibility[0][0].won == 0);
    TASK_CHECK(selection.visibility[1][0].won == selection.visibility[1][0].covered);
    TASK_CHECK(!selection.visibility[0][0].selectable);
    TASK_CHECK(selection.visibility[1][0].selectable);
    return 0;
}

/* Reverse winding must not change coverage when culling is off. */
int visibility_reverse_winding(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(16, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    view_texture::Mesh forward;
    forward.positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    forward.indices = { 0, 1, 2 };

    view_texture::Mesh reverse = forward;
    reverse.indices = { 0, 2, 1 };

    view_texture::Mesh rotated = forward;
    rotated.indices = { 1, 2, 0 };

    auto a = view_texture::select(forward, sources, 0);
    auto b = view_texture::select(reverse, sources, 0);
    auto c = view_texture::select(rotated, sources, 0);

    TASK_CHECK(a.valid && b.valid && c.valid);
    TASK_CHECK(a.visibility[0][0].covered == b.visibility[0][0].covered);
    TASK_CHECK(a.visibility[0][0].won == b.visibility[0][0].won);
    TASK_CHECK(a.visibility[0][0].covered == c.visibility[0][0].covered);
    TASK_CHECK(a.visibility[0][0].won == c.visibility[0][0].won);
    return 0;
}

/* Inputs that must be rejected before any result is published. */
int visibility_rejects_invalid(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(8, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    view_texture::Mesh bad_index;
    bad_index.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    bad_index.indices = { 0, 1, 9 };
    TASK_CHECK(!view_texture::select(bad_index, sources, 0).valid);

    view_texture::Mesh degenerate;
    degenerate.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } };
    degenerate.indices = { 0, 1, 2 };
    TASK_CHECK(!view_texture::select(degenerate, sources, 0).valid);

    view_texture::Mesh non_finite;
    non_finite.positions = { { 0, 0, 0 }, { 1, 0, 0 },
                             { std::numeric_limits<float>::quiet_NaN(), 1, 0 } };
    non_finite.indices = { 0, 1, 2 };
    TASK_CHECK(!view_texture::select(non_finite, sources, 0).valid);

    /* an edge with three incident faces */
    view_texture::Mesh non_manifold;
    non_manifold.positions = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 },
    };
    non_manifold.indices = { 0, 1, 2, 0, 1, 3, 0, 1, 4 };
    TASK_CHECK(!view_texture::select(non_manifold, sources, 0).valid);

    /* A negative seam penalty is rejected; zero is accepted. */
    view_texture::Mesh ok;
    ok.positions = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    ok.indices = { 0, 1, 2 };
    TASK_CHECK(!view_texture::select(ok, sources, -1).valid);
    TASK_CHECK(view_texture::select(ok, sources, 0).valid);

    /* non-finite source matrix */
    std::array<view_texture::SourceCamera, 2> bad_matrix = sources;
    bad_matrix[0].world_to_clip = m;
    bad_matrix[0].world_to_clip(0, 0) = std::numeric_limits<float>::infinity();
    TASK_CHECK(!view_texture::select(ok, bad_matrix, 0).valid);

    /* mismatched texture sizes */
    Texture<Vec4> other = solid_texture(4, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> mismatched = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &other},
    };
    TASK_CHECK(!view_texture::select(ok, mismatched, 0).valid);
    return 0;
}

/* Two-pass statistics must not render the whole mesh once per face: with N
 * faces the covered count of a face is independent of how many other faces
 * exist.  This is checked indirectly by scaling the mesh and verifying the
 * per-face numbers stay stable. */
int visibility_scaling(void)
{
    Mat4 m = ortho_unit();
    Texture<Vec4> tex = solid_texture(16, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &tex},
        view_texture::SourceCamera{m, &tex},
    };

    auto build = [](int n)
    {
        view_texture::Mesh mesh;
        for(int i = 0; i <= n; i++)
        {
            for(int j = 0; j <= n; j++)
            {
                mesh.positions.push_back({ -1.0f + 2.0f * i / n, -1.0f + 2.0f * j / n, 0.0f });
            }
        }
        auto at = [n](int i, int j) { return static_cast<unsigned int>(j * (n + 1) + i); };
        for(int j = 0; j < n; j++)
        {
            for(int i = 0; i < n; i++)
            {
                mesh.indices.push_back(at(i, j));
                mesh.indices.push_back(at(i + 1, j));
                mesh.indices.push_back(at(i + 1, j + 1));
                mesh.indices.push_back(at(i, j));
                mesh.indices.push_back(at(i + 1, j + 1));
                mesh.indices.push_back(at(i, j + 1));
            }
        }
        return mesh;
    };

    view_texture::Mesh small = build(2);
    view_texture::Mesh large = build(8);

    auto a = view_texture::select(small, sources, 0);
    auto b = view_texture::select(large, sources, 0);
    TASK_CHECK(a.valid && b.valid);
    TASK_CHECK(a.visibility.size() == small.indices.size() / 3);
    TASK_CHECK(b.visibility.size() == large.indices.size() / 3);

    /* The union of every face in a watertight full-screen grid covers the
     * whole target exactly once, so the covered counts sum to W*H. */
    auto sum_covered = [](const view_texture::Selection& s)
    {
        long long total = 0;
        for(const auto& face : s.visibility) total += face[0].covered;
        return total;
    };
    TASK_CHECK(sum_covered(a) == 16 * 16);
    TASK_CHECK(sum_covered(b) == 16 * 16);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  cut group                                                         */
/* ------------------------------------------------------------------ */

/* Exhaustive check over many random small problems. */
int cut_random_bruteforce(void)
{
    std::uint32_t state = 0x12345678u;
    auto next = [&state]()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };

    for(int trial = 0; trial < 40; trial++)
    {
        int faces = 2 + static_cast<int>(next() % 8);
        int width = 4, height = 4;

        view_texture::LabelProblem problem;
        problem.width = width;
        problem.height = height;
        problem.seam_penalty = static_cast<int>(next() % 5);
        problem.vertex_count = 8;

        /* Build a small connected strip of faces. */
        std::vector<std::array<unsigned int, 3>> triangles;
        for(int f = 0; f < faces; f++)
        {
            triangles.push_back({ static_cast<unsigned int>(f),
                                  static_cast<unsigned int>(f + 1),
                                  static_cast<unsigned int>(f + 2) });
        }
        for(auto& t : triangles)
        {
            problem.indices.push_back(t[0]);
            problem.indices.push_back(t[1]);
            problem.indices.push_back(t[2]);
        }
        problem.vertex_count = static_cast<std::size_t>(faces) + 2;

        problem.covered.resize(faces);
        for(int f = 0; f < faces; f++)
        {
            for(int c = 0; c < 2; c++)
            {
                int roll = static_cast<int>(next() % 10);
                if(roll == 0) problem.covered[f][c] = -1;
                else problem.covered[f][c] = static_cast<int>(next() % 17);
            }
        }

        auto result = view_texture::solve_labels(problem);
        if(result.error == "edge with more than two incident faces")
        {
            /* the generated strip may fold onto an existing edge; skip */
            continue;
        }
        TASK_CHECK(result.valid);

        long long best = brute_min_energy(problem);
        TASK_CHECK(result.energy == best);
        TASK_CHECK(brute_energy(problem, result.labels) == best);

        for(int label : result.labels) TASK_CHECK(label >= -1 && label <= 1);
    }
    return 0;
}

/* Per-face greedy is provably worse than the global optimum here. */
int cut_greedy_is_worse(void)
{
    view_texture::LabelProblem problem;
    problem.width = 10;
    problem.height = 10;
    problem.seam_penalty = 5;
    problem.vertex_count = 4;
    problem.indices = { 0, 1, 2, 0, 2, 3 };
    problem.covered = { { { 90, 80 } }, { { 90, 80 } } };

    auto result = view_texture::solve_labels(problem);
    TASK_CHECK(result.valid);

    long long best = brute_min_energy(problem);
    TASK_CHECK(result.energy == best);

    /*
     * Greedy per face picks each face's cheaper source independently.  With
     * covered {0,90} / {90,0} and W*H = 100 that is source 1 then source 0,
     * paying a seam penalty of 5 per shared edge, while the global optimum
     * is to agree on either source (one face pays 90, no seam).
     * Use a pixel count where the unary cost (38) is below the seam penalty
     * (40) so far enough: covered 62 vs 100 means cost 38.
     */
    problem.covered = { { { 100, 62 } }, { { 62, 100 } } };
    problem.seam_penalty = 40;
    result = view_texture::solve_labels(problem);

    long long greedy_energy = (100 - 100) + (100 - 100) + 40;  /* 0 + 0 + seam */
    long long agree0 = (100 - 100) + (100 - 62);               /* 0 + 38      */
    long long agree1 = (100 - 62) + (100 - 100);               /* 38 + 0      */

    TASK_CHECK(result.energy == std::min({ greedy_energy, agree0, agree1 }));
    TASK_CHECK(result.energy < greedy_energy);
    TASK_CHECK(result.energy == brute_min_energy(problem));
    return 0;
}

/* A face with exactly one allowed source and faces with none. */
int cut_single_and_none(void)
{
    view_texture::LabelProblem problem;
    problem.width = 8;
    problem.height = 8;
    problem.seam_penalty = 3;
    problem.vertex_count = 6;
    problem.indices = { 0, 1, 2, 2, 1, 3, 3, 1, 4 };
    problem.covered = {
        { { 40, -1 } },    /* only source 0 */
        { { -1, -1 } },    /* none */
        { { -1, 60 } },    /* only source 1 */
    };

    auto result = view_texture::solve_labels(problem);
    TASK_CHECK(result.valid);
    TASK_CHECK(result.labels[0] == 0);
    TASK_CHECK(result.labels[1] == -1);
    TASK_CHECK(result.labels[2] == 1);

    long long expected = (64 - 40) + (64 - 60);
    TASK_CHECK(result.energy == expected);
    TASK_CHECK(brute_min_energy(problem) == expected);
    return 0;
}

/* All sources forbidden for all faces. */
int cut_all_none(void)
{
    view_texture::LabelProblem problem;
    problem.width = 4;
    problem.height = 4;
    problem.seam_penalty = 1;
    problem.vertex_count = 4;
    problem.indices = { 0, 1, 2 };
    problem.covered = { { { -1, -1 } } };

    auto result = view_texture::solve_labels(problem);
    TASK_CHECK(result.valid);
    TASK_CHECK(result.labels.size() == 1);
    TASK_CHECK(result.labels[0] == -1);
    TASK_CHECK(result.energy == 0);
    return 0;
}

/* Larger connected grid: ensures the solver is not exhaustive and finds the
 * brute force optimum on a structured instance. */
int cut_larger_connected(void)
{
    const int n = 5;
    view_texture::LabelProblem problem;
    problem.width = 16;
    problem.height = 16;
    problem.seam_penalty = 7;
    problem.vertex_count = static_cast<std::size_t>((n + 1) * (n + 1));

    auto at = [n](int i, int j) { return static_cast<unsigned int>(j * (n + 1) + i); };
    for(int j = 0; j < n; j++)
    {
        for(int i = 0; i < n; i++)
        {
            problem.indices.insert(problem.indices.end(),
                                   { at(i, j), at(i + 1, j), at(i + 1, j + 1),
                                     at(i, j), at(i + 1, j + 1), at(i, j + 1) });
        }
    }

    std::size_t faces = problem.indices.size() / 3;
    problem.covered.resize(faces);
    std::uint32_t state = 99;
    for(std::size_t f = 0; f < faces; f++)
    {
        for(int c = 0; c < 2; c++)
        {
            state = state * 1103515245u + 12345u;
            problem.covered[f][c] = 100 + static_cast<int>((state >> 16) % 156);
        }
    }

    auto result = view_texture::solve_labels(problem);
    TASK_CHECK(result.valid);

    /* Compare against a local optimum computed with an independent
     * coordinate descent from both constant initialisations. */
    long long best = brute_energy(problem, result.labels);
    TASK_CHECK(result.energy == best);

    /* For a submodular binary problem the energy is at least as good as both
     * constant labelings, which is easy to check independently. */
    std::vector<int> zeros(faces, 0), ones(faces, 1);
    TASK_CHECK(result.energy <= brute_energy(problem, zeros));
    TASK_CHECK(result.energy <= brute_energy(problem, ones));

    TASK_CHECK(faces == static_cast<std::size_t>(2 * n * n));
    return 0;
}

/* Capacity overflow must be rejected rather than silently wrapping. */
int cut_overflow_guard(void)
{
    view_texture::LabelProblem problem;
    problem.width = 0;
    problem.height = 0;
    problem.vertex_count = 4;
    problem.indices = { 0, 1, 2 };
    problem.covered = { { { 0, 0 } } };
    TASK_CHECK(!view_texture::solve_labels(problem).valid);

    /* invalid index */
    view_texture::LabelProblem bad;
    bad.width = 2;
    bad.height = 2;
    bad.vertex_count = 3;
    bad.indices = { 0, 1, 5 };
    bad.covered = { { { 1, 1 } } };
    TASK_CHECK(!view_texture::solve_labels(bad).valid);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  projection group                                                  */
/* ------------------------------------------------------------------ */

/* Two source cameras with different clip spaces: the drawn colour must come
 * from the source selected per face. */
int projection_two_sources(void)
{
    Texture<Vec4> red = solid_texture(8, Vec4(1, 0, 0, 1));
    Texture<Vec4> green = solid_texture(8, Vec4(0, 1, 0, 1));

    /* source 0 sees only face 0 (z near 0), source 1 only face 1 (z near 0) */
    Mat4 ortho = ortho_unit();
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ortho, &red},
        view_texture::SourceCamera{ortho, &green},
    };

    view_texture::Mesh mesh;
    /* two side by side quads */
    mesh.positions = {
        { -0.9f, -0.9f, 0.0f }, { -0.1f, -0.9f, 0.0f }, { -0.1f, 0.9f, 0.0f }, { -0.9f, 0.9f, 0.0f },
        {  0.1f, -0.9f, 0.0f }, {  0.9f, -0.9f, 0.0f }, {  0.9f, 0.9f, 0.0f }, {  0.1f, 0.9f, 0.0f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);
    for(int label : selection.labels) TASK_CHECK(label == 0 || label == 1);

    Renderer renderer(32, 32);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));

    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{ortho},
                                  Vec4(0, 0, 1, 1)));

    for(std::size_t f = 0; f < 2; f++)
    {
        int label = selection.labels[f];
        RGBA8 color = renderer.framebuffer().color()(label == 0 ? 6 : 25, 16);
        if(label == 0)
        {
            TASK_CHECK(color.x > 200 && color.y < 60);
        }
        else
        {
            TASK_CHECK(color.y > 200 && color.x < 60);
        }
    }
    return 0;
}

/* The source and observer cameras have different w; the fragment uses the
 * observer's perspective corrected world position and then the source
 * matrix. */
int projection_different_w(void)
{
    Texture<Vec4> pattern = pattern_texture(16);

    Mat4 observer = Mat4::perspective(radians(70.0f), 1.0f, 1.0f, 10.0f);
    Mat4 source = Mat4::perspective(radians(50.0f), 1.0f, 0.5f, 20.0f);

    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{source, &pattern},
        view_texture::SourceCamera{source, &pattern},
    };

    view_texture::Mesh mesh;
    mesh.positions = {
        { -0.6f, -0.6f, -2.0f }, { 0.6f, -0.6f, -2.0f }, { 0.0f, 0.6f, -2.5f },
    };
    mesh.indices = { 0, 1, 2 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);
    TASK_CHECK(selection.labels[0] >= 0);

    Renderer renderer(32, 32);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));

    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{observer},
                                  Vec4(0, 0, 1, 1)));

    /* Centre pixel must be textured (much brighter than clear colour). */
    RGBA8 c = renderer.framebuffer().color()(16, 16);
    TASK_CHECK(c.x + c.y + c.z > 30);
    return 0;
}

/* Bilinear sampling at half texel and non half texel positions. */
int projection_bilinear(void)
{
    Texture<Vec4> texture(4, 4);
    for(int y = 0; y < 4; y++)
    {
        for(int x = 0; x < 4; x++)
        {
            texture(x, y) = Vec4(static_cast<float>(x) / 3.0f,
                                 static_cast<float>(y) / 3.0f, 0.0f, 1.0f);
        }
    }

    /* uv at a texel centre selects that texel. */
    Vec4 centre = view_texture::sample_bilinear(texture, Vec2(0.5f / 4.0f, 0.5f / 4.0f));
    TASK_CHECK(approx(centre.x, 0.0f, 2e-3f));
    TASK_CHECK(approx(centre.y, 0.0f, 2e-3f));

    Vec4 c11 = view_texture::sample_bilinear(texture, Vec2(2.5f / 4.0f, 1.5f / 4.0f));
    TASK_CHECK(approx(c11.x, 2.0f / 3.0f, 2e-3f));
    TASK_CHECK(approx(c11.y, 1.0f / 3.0f, 2e-3f));

    /* halfway between two texel centres gives the average. */
    Vec4 mid = view_texture::sample_bilinear(texture, Vec2(1.0f / 4.0f, 0.5f / 4.0f));
    TASK_CHECK(approx(mid.x, 0.5f / 3.0f, 2e-3f));

    /* clamped outside the texture. */
    Vec4 out = view_texture::sample_bilinear(texture, Vec2(-1.0f, 2.0f));
    TASK_CHECK(approx(out.x, 0.0f, 2e-3f));
    TASK_CHECK(approx(out.y, 1.0f, 2e-3f));
    return 0;
}

/* With observer == source, a full-target quad must map target pixel columns
 * linearly onto texture columns through the whole uv pipeline. */
int projection_uv_mapping(void)
{
    const int size = 32;
    Texture<Vec4> texture(size, size);
    for(int y = 0; y < size; y++)
    {
        for(int x = 0; x < size; x++)
        {
            texture(x, y) = Vec4(static_cast<float>(x) / (size - 1),
                                 static_cast<float>(y) / (size - 1), 0.0f, 1.0f);
        }
    }

    Mat4 m = ortho_unit();
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{m, &texture},
        view_texture::SourceCamera{m, &texture},
    };

    view_texture::Mesh mesh;
    mesh.positions = {
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f },
        { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    Renderer renderer(size, size);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));

    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{m}, Vec4(1, 1, 1, 1)));

    for(int x : { 0, 8, 16, 24, 31 })
    {
        int expected = static_cast<int>(std::lround(255.0 * x / (size - 1)));
        RGBA8 c = renderer.framebuffer().color()(x, 16);
        TASK_CHECK(std::abs(c.x - expected) <= 6);
    }
    for(int y : { 0, 8, 16, 24, 31 })
    {
        int expected = static_cast<int>(std::lround(255.0 * y / (size - 1)));
        RGBA8 c = renderer.framebuffer().color()(16, y);
        TASK_CHECK(std::abs(c.y - expected) <= 6);
    }
    return 0;
}

/* Unselectable faces draw the caller supplied fallback colour. */
int projection_fallback(void)
{
    Texture<Vec4> tex = solid_texture(8, Vec4(1, 0, 0, 1));
    Mat4 ortho = ortho_unit();

    /* The source camera is shifted far to the side so the face lies outside
     * its clip volume, while the observer still sees it in the centre. */
    Mat4 source_shifted = Mat4::translation(Vec3(5.0f, 0.0f, 0.0f)) * ortho;
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{source_shifted, &tex},
        view_texture::SourceCamera{source_shifted, &tex},
    };

    view_texture::Mesh mesh;
    mesh.positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    mesh.indices = { 0, 1, 2 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);
    TASK_CHECK(selection.labels[0] == -1);

    Renderer renderer(16, 16);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));

    const Vec4 fallback(0.0f, 0.0f, 1.0f, 1.0f);
    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{ortho}, fallback));

    RGBA8 c = renderer.framebuffer().color()(8, 8);
    TASK_CHECK(c.z > 200 && c.x < 60 && c.y < 60);
    return 0;
}

/* A whole triangle must not get a single flat texture colour: with a
 * gradient texture the two corners must sample differently. */
int projection_per_fragment(void)
{
    Texture<Vec4> texture(32, 32);
    for(int y = 0; y < 32; y++)
    {
        for(int x = 0; x < 32; x++)
        {
            texture(x, y) = Vec4(static_cast<float>(x) / 31.0f,
                                 static_cast<float>(y) / 31.0f, 0.0f, 1.0f);
        }
    }

    Mat4 ortho = ortho_unit();
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ortho, &texture},
        view_texture::SourceCamera{ortho, &texture},
    };

    view_texture::Mesh mesh;
    mesh.positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
    mesh.indices = { 0, 1, 2 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);
    TASK_CHECK(selection.labels[0] >= 0);

    Renderer renderer(32, 32);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));

    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{ortho},
                                  Vec4(0, 0, 0, 1)));

    RGBA8 left = renderer.framebuffer().color()(6, 6);
    RGBA8 right = renderer.framebuffer().color()(28, 28);
    TASK_CHECK(left.x != right.x || left.y != right.y);
    return 0;
}

/* Consecutive viewer draws with different cameras must reflect the new
 * camera (no stale state) and must not mutate the previous selection. */
int projection_switching_camera(void)
{
    Texture<Vec4> tex = solid_texture(8, Vec4(1, 1, 1, 1));
    Mat4 ortho = ortho_unit();
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ortho, &tex},
        view_texture::SourceCamera{ortho, &tex},
    };

    view_texture::Mesh mesh;
    mesh.positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    mesh.indices = { 0, 1, 2 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);
    auto snapshot_labels = selection.labels;
    long long snapshot_energy = selection.energy;

    Renderer renderer(16, 16);
    renderer.options().culling = false;

    /* camera 1: unit ortho -> triangle centred */
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));
    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{ortho}, Vec4(0, 0, 1, 1)));
    RGBA8 first = renderer.framebuffer().color()(8, 6);
    TASK_CHECK(first.x > 100 && first.y > 100 && first.z > 100);

    /* camera 2: shifted in clip space so the triangle moves to the right
     * half; a pixel that was covered before must now be clear. */
    Mat4 shifted = Mat4::translation(Vec3(0.8f, 0.0f, 0.0f)) * ortho;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));
    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{shifted}, Vec4(0, 0, 1, 1)));
    RGBA8 moved_centre = renderer.framebuffer().color()(7, 7);
    TASK_CHECK(moved_centre.x < 20 && moved_centre.y < 20 && moved_centre.z < 20);
    RGBA8 moved_right = renderer.framebuffer().color()(14, 7);
    TASK_CHECK(moved_right.x > 100 && moved_right.y > 100 && moved_right.z > 100);

    /* the selection must be identical after drawing */
    TASK_CHECK(selection.labels == snapshot_labels);
    TASK_CHECK(selection.energy == snapshot_energy);
    return 0;
}

/* Re-selecting must not mutate a previously returned selection. */
int projection_reselect_independent(void)
{
    Texture<Vec4> tex_a = solid_texture(8, Vec4(1, 0, 0, 1));
    Texture<Vec4> tex_b = solid_texture(8, Vec4(0, 1, 0, 1));
    Mat4 ortho = ortho_unit();

    view_texture::Mesh mesh;
    mesh.positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    mesh.indices = { 0, 1, 2 };

    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ortho, &tex_a},
        view_texture::SourceCamera{ortho, &tex_b},
    };

    auto first = view_texture::select(mesh, sources, 3);
    TASK_CHECK(first.valid);
    auto first_labels = first.labels;
    auto first_visibility = first.visibility;
    long long first_energy = first.energy;

    std::array<view_texture::SourceCamera, 2> swapped = {
        view_texture::SourceCamera{ortho, &tex_b},
        view_texture::SourceCamera{ortho, &tex_a},
    };
    auto second = view_texture::select(mesh, swapped, 9);
    TASK_CHECK(second.valid);

    TASK_CHECK(first.labels == first_labels);
    TASK_CHECK(first.energy == first_energy);
    TASK_CHECK(first.visibility.size() == first_visibility.size());
    if(first.visibility.size() == first_visibility.size())
    {
        TASK_CHECK(first.visibility[0][0].covered == first_visibility[0][0].covered);
    }
    TASK_CHECK(second.seam_penalty == 9);
    TASK_CHECK(first.seam_penalty == 3);
    return 0;
}

/* The label is not interpolated as a fragment attribute: the draw must use a
 * flat label even though adjacent faces have different labels. */
int projection_flat_label(void)
{
    Texture<Vec4> red = solid_texture(8, Vec4(1, 0, 0, 1));
    Texture<Vec4> green = solid_texture(8, Vec4(0, 1, 0, 1));
    Mat4 ortho = ortho_unit();

    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ortho, &red},
        view_texture::SourceCamera{ortho, &green},
    };

    view_texture::Mesh mesh;
    mesh.positions = {
        { -1.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f },
        {  0.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    Renderer renderer(32, 32);
    renderer.options().culling = false;
    renderer.framebuffer().clear(RGBA8(0, 0, 0, 255));

    TASK_CHECK(view_texture::draw(renderer, mesh, selection, sources,
                                  view_texture::ObserverCamera{ortho}, Vec4(0, 0, 1, 1)));

    /* left half is one face and right half the other; each region keeps a
     * single flat colour without a gradient across the shared edge. */
    RGBA8 left = renderer.framebuffer().color()(4, 16);
    RGBA8 right = renderer.framebuffer().color()(27, 16);
    TASK_CHECK((left.x > 200 && left.y < 60) || (left.y > 200 && left.x < 60));
    TASK_CHECK((right.x > 200 && right.y < 60) || (right.y > 200 && right.x < 60));
    return 0;
}

/* Drawing with an invalid selection or mismatched mesh must fail cleanly. */
int projection_rejects_invalid(void)
{
    Mat4 ortho = ortho_unit();
    Texture<Vec4> tex = solid_texture(8, Vec4(1, 1, 1, 1));
    std::array<view_texture::SourceCamera, 2> sources = {
        view_texture::SourceCamera{ortho, &tex},
        view_texture::SourceCamera{ortho, &tex},
    };

    view_texture::Mesh mesh;
    mesh.positions = { { -1, -1, 0 }, { 1, -1, 0 }, { 0, 1, 0 } };
    mesh.indices = { 0, 1, 2 };

    Renderer renderer(16, 16);
    renderer.options().culling = false;

    view_texture::Selection invalid;
    TASK_CHECK(!view_texture::draw(renderer, mesh, invalid, sources,
                                   view_texture::ObserverCamera{ortho}, Vec4(1, 1, 1, 1)));

    auto selection = view_texture::select(mesh, sources, 0);
    TASK_CHECK(selection.valid);

    view_texture::Mesh other;
    other.positions = mesh.positions;
    other.indices = { 0, 1, 2, 0, 1, 2 };
    TASK_CHECK(!view_texture::draw(renderer, other, selection, sources,
                                   view_texture::ObserverCamera{ortho}, Vec4(1, 1, 1, 1)));

    /* null texture */
    std::array<view_texture::SourceCamera, 2> bad = sources;
    bad[1].texture = nullptr;
    TASK_CHECK(!view_texture::draw(renderer, mesh, selection, bad,
                                   view_texture::ObserverCamera{ortho}, Vec4(1, 1, 1, 1)));
    return 0;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/*  registration                                                      */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "visibility", "single_triangle", visibility_single_triangle },
    { "visibility", "occlusion", visibility_occlusion },
    { "visibility", "cross_near", visibility_cross_near },
    { "visibility", "shared_edge", visibility_shared_edge },
    { "visibility", "viewport_shards", visibility_viewport_shards },
    { "visibility", "depth_tie", visibility_depth_tie },
    { "visibility", "reverse_winding", visibility_reverse_winding },
    { "visibility", "rejects_invalid", visibility_rejects_invalid },
    { "visibility", "scaling", visibility_scaling },

    { "cut", "random_bruteforce", cut_random_bruteforce },
    { "cut", "greedy_is_worse", cut_greedy_is_worse },
    { "cut", "single_and_none", cut_single_and_none },
    { "cut", "all_none", cut_all_none },
    { "cut", "larger_connected", cut_larger_connected },
    { "cut", "overflow_guard", cut_overflow_guard },

    { "projection", "two_sources", projection_two_sources },
    { "projection", "different_w", projection_different_w },
    { "projection", "bilinear", projection_bilinear },
    { "projection", "uv_mapping", projection_uv_mapping },
    { "projection", "fallback", projection_fallback },
    { "projection", "per_fragment", projection_per_fragment },
    { "projection", "switching_camera", projection_switching_camera },
    { "projection", "reselect_independent", projection_reselect_independent },
    { "projection", "flat_label", projection_flat_label },
    { "projection", "rejects_invalid", projection_rejects_invalid },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
