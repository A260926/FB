#include "view_texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <vector>

namespace view_texture
{

namespace
{

/* ------------------------------------------------------------------ */
/*  small double precision helpers (float inputs, double arithmetic)  */
/* ------------------------------------------------------------------ */

bool finite_vec3(const Vec3& p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

bool finite_mat(const Mat4& M)
{
    for(int j = 0; j < 4; j++)
    {
        for(int i = 0; i < 4; i++)
        {
            if(!std::isfinite(M(i, j))) return false;
        }
    }
    return true;
}

void matrix_d(const Mat4& M, double m[4][4])
{
    for(int r = 0; r < 4; r++)
    {
        for(int c = 0; c < 4; c++)
        {
            m[r][c] = static_cast<double>(M(r, c));
        }
    }
}

void transform_d(const double m[4][4], const Vec3& p, double out[4])
{
    out[0] = m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3];
    out[1] = m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3];
    out[2] = m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3];
    out[3] = m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3];
}

/* Clip planes of the volume -w <= x,y,z <= w with w > 0:
 *   sign * coord + w >= 0. */
struct Plane { int axis; double sign; };
constexpr Plane kPlanes[6] = {
    {0, +1.0}, {0, -1.0},
    {1, +1.0}, {1, -1.0},
    {2, +1.0}, {2, -1.0},
};

bool vertex_inside(const double clip[4])
{
    if(!(clip[3] > 0.0)) return false;
    for(const Plane& pl : kPlanes)
    {
        if(pl.sign * clip[pl.axis] + clip[3] < 0.0) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  half open coverage rule (y grows upwards)                         */
/* ------------------------------------------------------------------ */

inline double edge_eval(double ax, double ay, double bx, double by, double px, double py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* A directed edge accepts its boundary when it goes downwards, and when it
 * is horizontal going left.  Applied to consistently counter clockwise
 * wound triangles this is a watertight top-left rule. */
inline bool accept_edge(double e, double dx, double dy)
{
    double scale = std::max(1.0, std::fabs(dx) + std::fabs(dy));
    double eps = 1e-10 * scale;

    if(e > eps) return true;
    if(e < -eps) return false;

    if(dy < -eps) return true;
    if(dy > eps) return false;
    return dx < -eps;
}

/* ------------------------------------------------------------------ */
/*  clipping                                                          */
/* ------------------------------------------------------------------ */

struct ClipWing
{
    double clip[4];
};

ClipWing make_wing(const ClipWing& a, const ClipWing& b, double t)
{
    ClipWing r;
    for(int k = 0; k < 4; k++)
    {
        r.clip[k] = a.clip[k] + (b.clip[k] - a.clip[k]) * t;
    }
    return r;
}

using Poly = std::vector<ClipWing>;

void clip_plane(const Poly& in, const Plane& pl, Poly& out)
{
    out.clear();
    if(in.empty()) return;

    for(std::size_t i = 0; i < in.size(); i++)
    {
        const ClipWing& a = in[i];
        const ClipWing& b = in[(i + 1) % in.size()];

        double da = pl.sign * a.clip[pl.axis] + a.clip[3];
        double db = pl.sign * b.clip[pl.axis] + b.clip[3];
        bool ia = da >= 0.0;
        bool ib = db >= 0.0;

        if(ia && ib)
        {
            out.push_back(b);
        }
        else if(ia && !ib)
        {
            double t = da / (da - db);
            out.push_back(make_wing(a, b, t));
        }
        else if(!ia && ib)
        {
            double t = da / (da - db);
            out.push_back(make_wing(a, b, t));
            out.push_back(b);
        }
    }
}

/* Clip one face against the six volume planes before the perspective divide
 * and fan triangulate the result.  Every shard keeps the original face's
 * attribute source and the set of original vertex indices it originated
 * from; the shard count per original face is tracked elsewhere. */
template<typename ShardFunc>
void clip_face(const ClipWing& v0, const ClipWing& v1, const ClipWing& v2, ShardFunc&& shard)
{
    Poly poly;
    Poly tmp;
    poly.push_back(v0);
    poly.push_back(v1);
    poly.push_back(v2);

    for(const Plane& pl : kPlanes)
    {
        clip_plane(poly, pl, tmp);
        poly.swap(tmp);
        if(poly.size() < 3) return;
    }

    for(std::size_t i = 1; i + 1 < poly.size(); i++)
    {
        shard(poly[0], poly[i], poly[i + 1]);
    }
}

/* Canonicalise a projected triangle to counter clockwise orientation in the
 * y up target raster so that coverage is independent of input winding.
 * order[] maps canonical positions back to the incoming a/b/c order. */
inline bool canonicalise(double& ax, double& ay, double& bx, double& by,
                         double& cx, double& cy, int order[3])
{
    double d = edge_eval(ax, ay, bx, by, cx, cy);
    order[0] = 0;
    order[1] = 1;
    order[2] = 2;
    if(d < 0.0)
    {
        std::swap(bx, cx);
        std::swap(by, cy);
        order[1] = 2;
        order[2] = 1;
        d = -d;
    }
    return d > 0.0;
}

void project(const ClipWing& w, int W, int H, double& x, double& y)
{
    x = (w.clip[0] / w.clip[3] + 1.0) * 0.5 * W;
    y = (w.clip[1] / w.clip[3] + 1.0) * 0.5 * H;
}

inline int clamp_floor(double v, int lo, int hi)
{
    double f = std::floor(v);
    if(f < lo) return lo;
    if(f > hi) return hi;
    return static_cast<int>(f);
}

inline int clamp_ceil(double v, int lo, int hi)
{
    double c = std::ceil(v);
    if(c < lo) return lo;
    if(c > hi) return hi;
    return static_cast<int>(c);
}

/* Rasterize fragments of one clipped shard.  Barycentric weights are given in
 * canonical order and the caller re-permutes attribute data with order[]. */
template<typename Fragment>
void rasterize_shard(const ClipWing& a, const ClipWing& b, const ClipWing& c,
                     int W, int H, Fragment&& fragment)
{
    if(!(a.clip[3] > 0.0) || !(b.clip[3] > 0.0) || !(c.clip[3] > 0.0)) return;

    double ax, ay, bx, by, cx, cy;
    project(a, W, H, ax, ay);
    project(b, W, H, bx, by);
    project(c, W, H, cx, cy);

    int order[3];
    if(!canonicalise(ax, ay, bx, by, cx, cy, order)) return;

    double D = edge_eval(ax, ay, bx, by, cx, cy);

    double minx = std::min({ax, bx, cx});
    double maxx = std::max({ax, bx, cx});
    double miny = std::min({ay, by, cy});
    double maxy = std::max({ay, by, cy});

    int x0 = clamp_floor(minx - 0.5, 0, W - 1);
    int x1 = clamp_ceil(maxx - 0.5, 0, W - 1);
    int y0 = clamp_floor(miny - 0.5, 0, H - 1);
    int y1 = clamp_ceil(maxy - 0.5, 0, H - 1);
    if(x0 > x1 || y0 > y1) return;

    double dx_ab = bx - ax, dy_ab = by - ay;
    double dx_bc = cx - bx, dy_bc = cy - by;
    double dx_ca = ax - cx, dy_ca = ay - cy;

    for(int y = y0; y <= y1; y++)
    {
        double py = y + 0.5;
        for(int x = x0; x <= x1; x++)
        {
            double px = x + 0.5;

            double e_c = edge_eval(ax, ay, bx, by, px, py);
            double e_a = edge_eval(bx, by, cx, cy, px, py);
            double e_b = edge_eval(cx, cy, ax, ay, px, py);

            if(!accept_edge(e_c, dx_ab, dy_ab)) continue;
            if(!accept_edge(e_a, dx_bc, dy_bc)) continue;
            if(!accept_edge(e_b, dx_ca, dy_ca)) continue;

            double l_a = e_a / D;
            double l_b = e_b / D;
            double l_c = e_c / D;
            double sum = l_a + l_b + l_c;
            if(!(sum > 0.0) || !std::isfinite(sum)) continue;

            Vec3 lambda(static_cast<float>(l_a / sum),
                        static_cast<float>(l_b / sum),
                        static_cast<float>(l_c / sum));

            fragment(x, y, lambda, order);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  source camera visibility statistics (one full mesh pass each)     */
/* ------------------------------------------------------------------ */

struct Contribution
{
    int face;
    double depth;
    int next;
};

struct SourceStats
{
    std::vector<int> covered;
    std::vector<int> won;
    std::vector<char> inside3;
    std::vector<int> shard_count;
};

SourceStats rasterize_source(const Mesh& mesh, const double m[4][4],
                             unsigned int width, unsigned int height)
{
    const std::size_t face_count = mesh.indices.size() / 3;
    const std::size_t vertex_count = mesh.positions.size();

    SourceStats stats;
    stats.covered.assign(face_count, 0);
    stats.won.assign(face_count, 0);
    stats.inside3.assign(face_count, 0);
    stats.shard_count.assign(face_count, 0);

    const int W = static_cast<int>(width);
    const int H = static_cast<int>(height);

    std::vector<std::array<double, 4>> vertex_clip(vertex_count);
    std::vector<char> vertex_in(vertex_count, 0);

    for(std::size_t i = 0; i < vertex_count; i++)
    {
        transform_d(m, mesh.positions[i], vertex_clip[i].data());
        vertex_in[i] = vertex_inside(vertex_clip[i].data()) ? 1 : 0;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<int> head(pixel_count, -1);
    std::vector<int> last_face(pixel_count, -1);
    std::vector<Contribution> contributions;

    for(std::size_t f = 0; f < face_count; f++)
    {
        unsigned int i0 = mesh.indices[f * 3 + 0];
        unsigned int i1 = mesh.indices[f * 3 + 1];
        unsigned int i2 = mesh.indices[f * 3 + 2];

        stats.inside3[f] = (vertex_in[i0] && vertex_in[i1] && vertex_in[i2]) ? 1 : 0;

        const unsigned int ids[3] = {i0, i1, i2};

        ClipWing wings[3];
        for(int k = 0; k < 3; k++)
        {
            for(int c = 0; c < 4; c++) wings[k].clip[c] = vertex_clip[ids[k]][c];
        }

        const int face_id = static_cast<int>(f);

        auto add_sample = [&](int x, int y, double z)
        {
            std::size_t idx = static_cast<std::size_t>(y) * W + x;

            /* Shards of one original face are processed contiguously, so a
             * per-pixel stamp makes the distinct-sample count O(1) instead
             * of scanning every contribution already stored at the pixel. */
            if(last_face[idx] == face_id) return;
            last_face[idx] = face_id;

            contributions.push_back({face_id, z, head[idx]});
            head[idx] = static_cast<int>(contributions.size() - 1);
            stats.covered[f] += 1;
        };

        clip_face(wings[0], wings[1], wings[2],
            [&](const ClipWing& a, const ClipWing& b, const ClipWing& c)
            {
                stats.shard_count[f] += 1;

                const ClipWing* wing[3] = {&a, &b, &c};
                double zz[3];
                for(int k = 0; k < 3; k++)
                {
                    zz[k] = 0.5 * (wing[k]->clip[2] / wing[k]->clip[3]) + 0.5;
                    zz[k] = std::clamp(zz[k], 0.0, 1.0);
                }

                rasterize_shard(a, b, c, W, H,
                    [&](int x, int y, const Vec3& lambda, const int order[3])
                    {
                        double z = lambda.x * zz[order[0]] +
                                   lambda.y * zz[order[1]] +
                                   lambda.z * zz[order[2]];
                        add_sample(x, y, z);
                    });
            });
    }

    /* Single depth resolve over the whole target; ties go to the later input
     * face. */
    const double tie = 1e-7;
    for(int y = 0; y < H; y++)
    {
        for(int x = 0; x < W; x++)
        {
            std::size_t idx = static_cast<std::size_t>(y) * W + x;
            if(head[idx] == -1) continue;

            double best_z = std::numeric_limits<double>::infinity();
            for(int e = head[idx]; e != -1; e = contributions[e].next)
            {
                best_z = std::min(best_z, contributions[e].depth);
            }

            int best_face = -1;
            for(int e = head[idx]; e != -1; e = contributions[e].next)
            {
                if(contributions[e].depth <= best_z + tie &&
                   contributions[e].face > best_face)
                {
                    best_face = contributions[e].face;
                }
            }

            if(best_face >= 0) stats.won[best_face] += 1;
        }
    }

    return stats;
}

/* ------------------------------------------------------------------ */
/*  exact binary min cut (Dinic)                                      */
/* ------------------------------------------------------------------ */

struct Dinic
{
    struct Edge
    {
        int to;
        int rev;
        long long cap;
    };

    std::vector<std::vector<Edge>> graph;
    std::vector<int> level;
    std::vector<int> iter;

    explicit Dinic(int n) : graph(n), level(n), iter(n) {}

    void add_directed(int from, int to, long long cap)
    {
        Edge a{to, static_cast<int>(graph[to].size()), cap};
        Edge b{from, static_cast<int>(graph[from].size()), 0};
        graph[from].push_back(a);
        graph[to].push_back(b);
    }

    void add_undirected(int u, int v, long long cap)
    {
        add_directed(u, v, cap);
        add_directed(v, u, cap);
    }

    bool bfs(int s, int t)
    {
        std::fill(level.begin(), level.end(), -1);
        std::queue<int> q;
        level[s] = 0;
        q.push(s);
        while(!q.empty())
        {
            int v = q.front();
            q.pop();
            for(const Edge& e : graph[v])
            {
                if(e.cap > 0 && level[e.to] < 0)
                {
                    level[e.to] = level[v] + 1;
                    q.push(e.to);
                }
            }
        }
        return level[t] >= 0;
    }

    long long dfs(int v, int t, long long f)
    {
        if(v == t) return f;
        for(int& i = iter[v]; i < static_cast<int>(graph[v].size()); i++)
        {
            Edge& e = graph[v][i];
            if(e.cap > 0 && level[v] < level[e.to])
            {
                long long d = dfs(e.to, t, std::min(f, e.cap));
                if(d > 0)
                {
                    e.cap -= d;
                    graph[e.to][e.rev].cap += d;
                    return d;
                }
            }
        }
        return 0;
    }

    long long max_flow(int s, int t)
    {
        long long flow = 0;
        const long long inf = std::numeric_limits<long long>::max() / 4;
        while(bfs(s, t))
        {
            std::fill(iter.begin(), iter.end(), 0);
            while(long long f = dfs(s, t, inf))
            {
                flow += f;
            }
        }
        return flow;
    }

    std::vector<char> reachable(int s) const
    {
        std::vector<char> seen(graph.size(), 0);
        std::queue<int> q;
        q.push(s);
        seen[s] = 1;
        while(!q.empty())
        {
            int v = q.front();
            q.pop();
            for(const Edge& e : graph[v])
            {
                if(e.cap > 0 && !seen[e.to])
                {
                    seen[e.to] = 1;
                    q.push(e.to);
                }
            }
        }
        return seen;
    }
};

/* Interior edges together with their two incident face indices. */
struct SeamEdge
{
    int face_a;
    int face_b;
};

std::vector<SeamEdge> seam_edges(const std::vector<unsigned int>& indices, std::size_t face_count)
{
    std::map<std::pair<unsigned int, unsigned int>, std::pair<int, std::array<int, 2>>> table;

    for(std::size_t f = 0; f < face_count; f++)
    {
        unsigned int vs[3] = {indices[f * 3 + 0], indices[f * 3 + 1], indices[f * 3 + 2]};
        for(int k = 0; k < 3; k++)
        {
            auto key = std::minmax(vs[k], vs[(k + 1) % 3]);
            auto it = table.find(key);
            if(it == table.end())
            {
                table.emplace(key, std::make_pair(1, std::array<int, 2>{static_cast<int>(f), -1}));
            }
            else
            {
                it->second.first += 1;
                if(it->second.second[1] < 0) it->second.second[1] = static_cast<int>(f);
            }
        }
    }

    std::vector<SeamEdge> result;
    for(const auto& entry : table)
    {
        if(entry.second.first == 2 && entry.second.second[1] >= 0)
        {
            result.push_back({entry.second.second[0], entry.second.second[1]});
        }
    }
    return result;
}

bool validate_problem(const LabelProblem& p, std::string& error)
{
    if(p.width == 0 || p.height == 0)
    {
        error = "empty texture dimensions";
        return false;
    }
    if(p.seam_penalty < 0)
    {
        error = "negative seam penalty";
        return false;
    }
    if(p.indices.size() % 3 != 0)
    {
        error = "index count not divisible by three";
        return false;
    }

    const std::size_t face_count = p.indices.size() / 3;
    if(p.covered.size() != face_count)
    {
        error = "covered statistics do not match face count";
        return false;
    }

    for(unsigned int index : p.indices)
    {
        if(index >= p.vertex_count)
        {
            error = "invalid vertex index";
            return false;
        }
    }

    const long long pixel_count = static_cast<long long>(p.width) * static_cast<long long>(p.height);
    for(std::size_t f = 0; f < face_count; f++)
    {
        for(int c = 0; c < 2; c++)
        {
            if(p.covered[f][c] < -1)
            {
                error = "covered statistic smaller than -1";
                return false;
            }
            if(p.covered[f][c] > pixel_count)
            {
                error = "covered statistic larger than target";
                return false;
            }
        }
    }

    std::map<std::pair<unsigned int, unsigned int>, int> edge_faces;
    for(std::size_t f = 0; f < face_count; f++)
    {
        unsigned int vs[3] = {
            p.indices[f * 3 + 0], p.indices[f * 3 + 1], p.indices[f * 3 + 2]};
        if(vs[0] == vs[1] || vs[1] == vs[2] || vs[2] == vs[0])
        {
            error = "degenerate face";
            return false;
        }
        for(int k = 0; k < 3; k++)
        {
            auto key = std::minmax(vs[k], vs[(k + 1) % 3]);
            int& n = edge_faces[key];
            n++;
            if(n > 2)
            {
                error = "edge with more than two incident faces";
                return false;
            }
        }
    }

    return true;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/*  public: exact labelling                                           */
/* ------------------------------------------------------------------ */

LabelResult solve_labels(const LabelProblem& problem)
{
    LabelResult result;

    std::string error;
    if(!validate_problem(problem, error))
    {
        result.error = error;
        return result;
    }

    const std::size_t face_count = problem.indices.size() / 3;

    /* The target pixel count bounds every unary cost; reject targets whose
     * product cannot be represented in a signed 64 bit integer. */
    const unsigned long long raw_pixels =
        static_cast<unsigned long long>(problem.width) * static_cast<unsigned long long>(problem.height);
    const long long limit = std::numeric_limits<long long>::max() / 4;
    if(raw_pixels > static_cast<unsigned long long>(limit))
    {
        result.error = "target dimensions overflow";
        return result;
    }

    const long long pixel_count = static_cast<long long>(raw_pixels);
    const long long penalty = problem.seam_penalty;

    /* Confirm that the worst case energy (and therefore every individual
     * capacity) fits a signed 64 bit integer without overflowing. */
    long long bound = 0;
    for(std::size_t f = 0; f < face_count; f++)
    {
        for(int c = 0; c < 2; c++)
        {
            int covered = problem.covered[f][c];
            if(covered < 0) continue;
            long long cost = pixel_count - covered;
            if(bound > limit - cost)
            {
                result.error = "energy capacity overflow";
                return result;
            }
            bound += cost;
        }
    }

    std::vector<SeamEdge> seams = seam_edges(problem.indices, face_count);
    if(!seams.empty())
    {
        if(penalty > 0 && static_cast<long long>(seams.size()) > (limit - bound) / penalty)
        {
            result.error = "energy capacity overflow";
            return result;
        }
        bound += penalty * static_cast<long long>(seams.size());
    }

    /* A face with exactly one allowed source has a fixed label.  Faces with
     * none are unlabelled (-1).  Only faces with two allowed sources become
     * graph nodes, so no "infinite" forcing arcs are needed and every
     * capacity stays bounded by the checked total. */
    std::vector<int> node_of_face(face_count, -1);
    std::vector<std::size_t> face_of_node;
    std::vector<int> fixed_label(face_count, -1);
    for(std::size_t f = 0; f < face_count; f++)
    {
        bool allowed0 = problem.covered[f][0] >= 0;
        bool allowed1 = problem.covered[f][1] >= 0;
        if(allowed0 && allowed1)
        {
            node_of_face[f] = static_cast<int>(face_of_node.size());
            face_of_node.push_back(f);
        }
        else if(allowed0)
        {
            fixed_label[f] = 0;
        }
        else if(allowed1)
        {
            fixed_label[f] = 1;
        }
    }

    const std::size_t node_count_faces = face_of_node.size();
    std::vector<long long> cost0(node_count_faces, 0);
    std::vector<long long> cost1(node_count_faces, 0);
    for(std::size_t n = 0; n < node_count_faces; n++)
    {
        std::size_t f = face_of_node[n];
        cost0[n] = pixel_count - problem.covered[f][0];
        cost1[n] = pixel_count - problem.covered[f][1];
    }

    std::vector<std::array<int, 2>> seam_nodes;   /* free-free seam pairs */
    for(const SeamEdge& edge : seams)
    {
        int la = fixed_label[edge.face_a];
        int lb = fixed_label[edge.face_b];
        bool fa = node_of_face[edge.face_a] >= 0;
        bool fb = node_of_face[edge.face_b] >= 0;

        /* At least one side has no selectable source: no seam term. */
        if(!fa && la < 0) continue;
        if(!fb && lb < 0) continue;

        if(fa && fb)
        {
            seam_nodes.push_back({node_of_face[edge.face_a], node_of_face[edge.face_b]});
        }
        else if(fa && !fb)
        {
            /* Neighbour has fixed label lb; the seam is paid when the free
             * face takes the opposite label. */
            if(lb == 0) cost1[node_of_face[edge.face_a]] += penalty;
            else        cost0[node_of_face[edge.face_a]] += penalty;
        }
        else if(!fa && fb)
        {
            if(la == 0) cost1[node_of_face[edge.face_b]] += penalty;
            else        cost0[node_of_face[edge.face_b]] += penalty;
        }
        else
        {
            /* two fixed faces: contributes only a constant to the objective,
             * which the final energy recomputation accounts for. */
        }
    }

    const int source = 0;
    const int sink = 1;
    const int node_count = 2 + static_cast<int>(node_count_faces);
    Dinic dinic(node_count);

    /*
     * Cut semantics: a face node on the source side has label 0, on the sink
     * side label 1.  Paying C(face, 0) means keeping the node on the sink
     * side (arc node -> sink); paying C(face, 1) means keeping the node on
     * the source side (arc source -> node).
     */
    for(std::size_t n = 0; n < node_count_faces; n++)
    {
        if(cost0[n] > limit || cost1[n] > limit)
        {
            result.error = "energy capacity overflow";
            return result;
        }
        dinic.add_directed(2 + static_cast<int>(n), sink, cost0[n]);
        dinic.add_directed(source, 2 + static_cast<int>(n), cost1[n]);
    }

    if(penalty > 0)
    {
        for(const auto& pair : seam_nodes)
        {
            dinic.add_undirected(2 + pair[0], 2 + pair[1], penalty);
        }
    }

    dinic.max_flow(source, sink);
    std::vector<char> seen = dinic.reachable(source);

    result.labels.assign(face_count, -1);
    for(std::size_t f = 0; f < face_count; f++)
    {
        int node = node_of_face[f];
        if(node >= 0)
        {
            /* source side of the cut is label 0, sink side label 1 */
            result.labels[f] = seen[2 + node] ? 0 : 1;
        }
        else if(fixed_label[f] >= 0)
        {
            result.labels[f] = fixed_label[f];
        }
    }

    /* Recompute the energy directly from the labels. */
    long long energy = 0;
    for(std::size_t f = 0; f < face_count; f++)
    {
        int label = result.labels[f];
        if(label < 0) continue;
        int covered = problem.covered[f][label];
        if(covered < 0)
        {
            /* Can normally not happen; fall back to the other source. */
            int other = 1 - label;
            if(problem.covered[f][other] >= 0)
            {
                result.labels[f] = other;
                label = other;
                covered = problem.covered[f][other];
            }
            else
            {
                result.labels[f] = -1;
                continue;
            }
        }
        energy += pixel_count - covered;
    }

    for(const SeamEdge& edge : seams)
    {
        int label_a = result.labels[edge.face_a];
        int label_b = result.labels[edge.face_b];
        if(label_a >= 0 && label_b >= 0 && label_a != label_b)
        {
            energy += penalty;
        }
    }

    if(energy > limit)
    {
        result.error = "energy overflow";
        result.labels.clear();
        return result;
    }

    result.energy = energy;
    result.valid = true;
    return result;
}

/* ------------------------------------------------------------------ */
/*  texture sampling                                                  */
/* ------------------------------------------------------------------ */

Vec4 sample_bilinear(const Texture<Vec4>& texture, const Vec2& uv)
{
    const int width = texture.width();
    const int height = texture.height();

    /* Keep the coordinate finite and within a range where the integer texel
     * indices cannot overflow before the per-texel clamp. */
    auto safe = [](float v, int size)
    {
        if(!std::isfinite(v)) return 0.0;
        const double limit = static_cast<double>(size) + 2.0;
        return std::clamp(static_cast<double>(v) * size - 0.5, -limit, limit);
    };

    const double cx = safe(uv.x, width);
    const double cy = safe(uv.y, height);

    const double fx = std::floor(cx);
    const double fy = std::floor(cy);

    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const float tx = static_cast<float>(cx - fx);
    const float ty = static_cast<float>(cy - fy);

    auto texel = [&](int x, int y)
    {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return texture(x, y);
    };

    const Vec4 c00 = texel(x0, y0);
    const Vec4 c10 = texel(x1, y0);
    const Vec4 c01 = texel(x0, y1);
    const Vec4 c11 = texel(x1, y1);

    const Vec4 c0 = c00 * (1.0f - tx) + c10 * tx;
    const Vec4 c1 = c01 * (1.0f - tx) + c11 * tx;
    return c0 * (1.0f - ty) + c1 * ty;
}

/* ------------------------------------------------------------------ */
/*  validation                                                        */
/* ------------------------------------------------------------------ */

namespace
{

bool validate_mesh(const Mesh& mesh, std::string& error)
{
    if(mesh.indices.size() % 3 != 0)
    {
        error = "index count not divisible by three";
        return false;
    }

    for(const Vec3& p : mesh.positions)
    {
        if(!finite_vec3(p))
        {
            error = "non-finite vertex position";
            return false;
        }
    }

    std::map<std::pair<unsigned int, unsigned int>, int> edge_faces;

    for(std::size_t f = 0; f < mesh.indices.size() / 3; f++)
    {
        unsigned int i0 = mesh.indices[f * 3 + 0];
        unsigned int i1 = mesh.indices[f * 3 + 1];
        unsigned int i2 = mesh.indices[f * 3 + 2];

        if(i0 >= mesh.positions.size() || i1 >= mesh.positions.size() || i2 >= mesh.positions.size())
        {
            error = "invalid vertex index";
            return false;
        }

        const Vec3& a = mesh.positions[i0];
        const Vec3& b = mesh.positions[i1];
        const Vec3& c = mesh.positions[i2];

        double e1x = static_cast<double>(b.x) - a.x;
        double e1y = static_cast<double>(b.y) - a.y;
        double e1z = static_cast<double>(b.z) - a.z;
        double e2x = static_cast<double>(c.x) - a.x;
        double e2y = static_cast<double>(c.y) - a.y;
        double e2z = static_cast<double>(c.z) - a.z;

        double nx = e1y * e2z - e1z * e2y;
        double ny = e1z * e2x - e1x * e2z;
        double nz = e1x * e2y - e1y * e2x;
        double len2 = nx * nx + ny * ny + nz * nz;
        if(!(len2 > 0.0) || !std::isfinite(len2))
        {
            error = "degenerate face";
            return false;
        }

        unsigned int vs[3] = {i0, i1, i2};
        for(int k = 0; k < 3; k++)
        {
            auto key = std::minmax(vs[k], vs[(k + 1) % 3]);
            int& n = edge_faces[key];
            n++;
            if(n > 2)
            {
                error = "edge with more than two incident faces";
                return false;
            }
        }
    }

    return true;
}

bool validate_sources(const std::array<SourceCamera, 2>& sources, std::string& error)
{
    for(int c = 0; c < 2; c++)
    {
        if(!finite_mat(sources[c].world_to_clip))
        {
            error = "non-finite source matrix";
            return false;
        }
        if(sources[c].texture == nullptr)
        {
            error = "missing source texture";
            return false;
        }
        const Texture<Vec4>& texture = *sources[c].texture;
        if(texture.width() <= 0 || texture.height() <= 0)
        {
            error = "empty source texture";
            return false;
        }
        for(const Vec4& v : texture.data())
        {
            if(!std::isfinite(v.x) || !std::isfinite(v.y) ||
               !std::isfinite(v.z) || !std::isfinite(v.w))
            {
                error = "non-finite source texture value";
                return false;
            }
        }
    }

    if(sources[0].texture->width() != sources[1].texture->width() ||
       sources[0].texture->height() != sources[1].texture->height())
    {
        error = "source textures differ in size";
        return false;
    }

    return true;
}

} /* namespace */

/* ------------------------------------------------------------------ */
/*  public: source selection                                          */
/* ------------------------------------------------------------------ */

Selection select(const Mesh& mesh, const std::array<SourceCamera, 2>& sources, int seam_penalty)
{
    Selection selection;

    if(seam_penalty < 0)
    {
        selection.error = "negative seam penalty";
        return selection;
    }

    std::string error;
    if(!validate_mesh(mesh, error))
    {
        selection.error = error;
        return selection;
    }
    if(!validate_sources(sources, error))
    {
        selection.error = error;
        return selection;
    }

    const unsigned int width = sources[0].texture->width();
    const unsigned int height = sources[0].texture->height();
    const std::size_t face_count = mesh.indices.size() / 3;

    selection.width = width;
    selection.height = height;
    selection.seam_penalty = seam_penalty;
    selection.visibility.assign(face_count, std::array<FaceVisibility, 2>{FaceVisibility{0, 0, false}, FaceVisibility{0, 0, false}});

    double matrices[2][4][4];
    for(int c = 0; c < 2; c++) matrix_d(sources[c].world_to_clip, matrices[c]);

    const SourceStats stats[2] = {
        rasterize_source(mesh, matrices[0], width, height),
        rasterize_source(mesh, matrices[1], width, height),
    };

    LabelProblem problem;
    problem.width = width;
    problem.height = height;
    problem.seam_penalty = seam_penalty;
    problem.vertex_count = mesh.positions.size();
    problem.indices = mesh.indices;
    problem.covered.resize(face_count);

    for(std::size_t f = 0; f < face_count; f++)
    {
        for(int c = 0; c < 2; c++)
        {
            FaceVisibility& v = selection.visibility[f][c];
            v.covered = stats[c].covered[f];
            v.won = stats[c].won[f];
            v.shards = stats[c].shard_count[f];
            v.selectable = stats[c].inside3[f] && v.covered > 0 && v.won == v.covered;
            problem.covered[f][c] = v.selectable ? v.covered : -1;
        }
    }

    LabelResult labels = solve_labels(problem);
    if(!labels.valid)
    {
        selection.error = labels.error.empty() ? "labelling failed" : labels.error;
        return selection;
    }

    selection.labels = std::move(labels.labels);
    selection.energy = labels.energy;
    selection.valid = true;
    return selection;
}

/* ------------------------------------------------------------------ */
/*  observer draw (through the ordinary Renderer triangle pipeline)   */
/* ------------------------------------------------------------------ */

bool draw(Renderer& renderer,
          const Mesh& mesh,
          const Selection& selection,
          const std::array<SourceCamera, 2>& sources,
          const ObserverCamera& observer,
          const Vec4& fallback_color)
{
    if(!selection.valid) return false;

    std::string error;
    if(!validate_mesh(mesh, error)) return false;

    const std::size_t face_count = mesh.indices.size() / 3;
    if(selection.labels.size() != face_count) return false;
    if(selection.width == 0 || selection.height == 0) return false;

    for(int c = 0; c < 2; c++)
    {
        if(sources[c].texture == nullptr) return false;
        if(sources[c].texture->width() != static_cast<int>(selection.width)) return false;
        if(sources[c].texture->height() != static_cast<int>(selection.height)) return false;
    }

    if(!finite_mat(observer.world_to_clip)) return false;
    if(!std::isfinite(fallback_color.x) || !std::isfinite(fallback_color.y) ||
       !std::isfinite(fallback_color.z) || !std::isfinite(fallback_color.w))
    {
        return false;
    }

    for(int label : selection.labels)
    {
        if(label < -1 || label > 1) return false;
    }

    struct Vertex
    {
        Vec3 world;
    };

    struct Varying
    {
        Vec4 position;
        Vec3 world;

        VARYING(position, world);
    };

    struct Uniforms
    {
        Mat4 observer;
        Mat4 source;
        const Texture<Vec4>* texture = nullptr;
        Vec4 fallback;
        bool textured = false;
    };

    Program<Vertex, Varying, Uniforms> program;

    program.onVertex([](const Uniforms& u, const Vertex& in, Varying& out)
    {
        out.position = u.observer * Vec4(in.world, 1.0f);
        out.world = in.world;
    });

    program.onFragment([](const Uniforms& u, const Varying& in, Vec4& out)
    {
        out = u.fallback;
        if(!u.textured || u.texture == nullptr) return;

        Vec4 clip = u.source * Vec4(in.world, 1.0f);
        if(!(clip.w > 1e-12f)) return;

        Vec2 uv((clip.x / clip.w + 1.0f) * 0.5f,
                (clip.y / clip.w + 1.0f) * 0.5f);
        out = sample_bilinear(*u.texture, uv);
    });

    Uniforms uniforms;
    uniforms.observer = observer.world_to_clip;
    uniforms.fallback = fallback_color;

    /* One draw call per original face keeps the input order for depth ties
     * and keeps the flat source label in a uniform instead of a varying. */
    for(std::size_t f = 0; f < face_count; f++)
    {
        const int label = selection.labels[f];

        Buffer<Vertex> buffer;
        buffer.primitive = ePrimitive::TRIANGLES;
        buffer.vertices.resize(3);
        for(int k = 0; k < 3; k++)
        {
            buffer.vertices[k].world = mesh.positions[mesh.indices[f * 3 + k]];
        }

        if(label >= 0)
        {
            uniforms.source = sources[label].world_to_clip;
            uniforms.texture = sources[label].texture;
            uniforms.textured = true;
        }
        else
        {
            uniforms.texture = nullptr;
            uniforms.textured = false;
        }

        program.uniforms() = uniforms;
        renderer.draw(program, buffer);
    }

    return true;
}

} /* namespace view_texture */
