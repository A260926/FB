//=====================================================================
//
// tests/moment_oit.cpp - 矩透明离屏帧测试
//
// 三组用例（capture / reconstruct / lifecycle）全部通过真实
// RenderHelp::DrawPrimitive 走完整 VS/PS、裁剪、覆盖与属性遍历，
// 期望值由本文件内独立的参考实现重新计算，不使用预填矩反调求解器。
//
// 不定义 main：由 task_support/driver.c 驱动。
//
//=====================================================================
#include "task_support/check.h"

#include "moment_oit.h"

#include <cmath>
#include <vector>
#include <memory>
#include <limits>
#include <stdexcept>
#include <algorithm>


namespace {

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double PI_TOL = 1e-7;      // 普通尺度绝对误差
// 提交顺序互换只允许浮点累加误差；矩的 ulp 级差异经非线性求积求解后
// 放大到 1e-12 量级，取 1e-10 留余量，仍比精度门槛严三个数量级
constexpr double ORDER_TOL = 1e-10;


//---------------------------------------------------------------------
// 测试几何：三个角的齐次坐标与颜色（属性槽在三个角上一致）
//---------------------------------------------------------------------
struct Geom {
    Vec4f p[3];
    Vec4f c[3];
};

// 覆盖整块屏幕的三角形，深度/颜色可逐角指定
Geom fullscreen(const float d[3], const Vec4f& c0,
                const Vec4f& c1, const Vec4f& c2, float w = 1.0f) {
    Geom g;
    g.p[0] = Vec4f(-w, -w, d[0] * w, w);
    g.p[1] = Vec4f(3.0f * w, -w, d[1] * w, w);
    g.p[2] = Vec4f(-w, 3.0f * w, d[2] * w, w);
    g.c[0] = c0; g.c[1] = c1; g.c[2] = c2;
    return g;
}

Geom fullscreenConst(float depth, const Vec4f& c, float w = 1.0f) {
    float d[3] = { depth, depth, depth };
    return fullscreen(d, c, c, c, w);
}

// 在 RenderHelp 上安装共享的 VS/PS，计数器记录真实调用次数
void install(RenderHelp& rh, const Geom& g, int& vsc, int& psc) {
    rh.SetVertexShader([&, g](int i, ShaderContext& o) -> Vec4f {
        ++vsc;
        o.varying_vec4f[0] = g.c[i];
        return g.p[i];
    });
    rh.SetPixelShader([&](ShaderContext& in) -> Vec4f {
        ++psc;
        return in.varying_vec4f[0];
    });
}


//---------------------------------------------------------------------
// 独立参考实现：屏幕重心 + 透视矫正，与生产代码分开推导
//---------------------------------------------------------------------
struct RefVertex {
    double sx, sy, nx, ny, depth, rhw;
    double col[4];
};

struct RefTri {
    explicit RefTri(const Geom& g, int W, int H) {
        for (int i = 0; i < 3; i++) {
            double w = g.p[i].w;
            v[i].nx = g.p[i].x / w;
            v[i].ny = g.p[i].y / w;
            v[i].sx = (v[i].nx + 1.0) * W * 0.5;
            v[i].sy = (1.0 - v[i].ny) * H * 0.5;
            v[i].depth = g.p[i].z / w;
            v[i].rhw = 1.0 / w;
            for (int k = 0; k < 4; k++) v[i].col[k] = g.c[i][k];
        }
    }
    RefVertex v[3];

    // 像素中心是否落在（未裁剪的）投影三角形内，返回透视矫正后的属性
    bool bary(int px, int py, double lam[3], double pc[4],
              double& depth, double& nx, double& ny) const {
        double cx = px + 0.5, cy = py + 0.5;
        double d = (v[1].sx - v[0].sx) * (v[2].sy - v[0].sy)
                 - (v[1].sy - v[0].sy) * (v[2].sx - v[0].sx);
        if (d == 0.0) return false;
        double a = ((v[1].sx - cx) * (v[2].sy - cy)
                  - (v[1].sy - cy) * (v[2].sx - cx)) / d;
        double b = ((v[2].sx - cx) * (v[0].sy - cy)
                  - (v[2].sy - cy) * (v[0].sx - cx)) / d;
        double c = 1.0 - a - b;
        const double eps = 1e-9;
        if (a < -eps || b < -eps || c < -eps) return false;
        lam[0] = a; lam[1] = b; lam[2] = c;
        double r = a * v[0].rhw + b * v[1].rhw + c * v[2].rhw;
        depth = a * v[0].depth + b * v[1].depth + c * v[2].depth;
        nx = a * v[0].nx + b * v[1].nx + c * v[2].nx;
        ny = a * v[0].ny + b * v[1].ny + c * v[2].ny;
        for (int k = 0; k < 4; k++) {
            pc[k] = (a * v[0].rhw * v[0].col[k]
                   + b * v[1].rhw * v[1].col[k]
                   + c * v[2].rhw * v[2].col[k]) / r;
        }
        return true;
    }

    // 完整裁剪体 x/y∈[-1,1]、z∈[0,1] 内的覆盖
    bool covered(int px, int py, double pc[4], double& depth) const {
        double lam[3], nx, ny;
        if (!bary(px, py, lam, pc, depth, nx, ny)) return false;
        if (nx < -1.0 || nx > 1.0 || ny < -1.0 || ny > 1.0) return false;
        if (depth < 0.0 || depth > 1.0) return false;
        return true;
    }
};

struct RefPixel {
    double b[4] = { 0, 0, 0, 0 };
    double g[6] = { 0, 0, 0, 0, 0, 0 };
    double sealed[3];
    double sealedDepth = INF;
};

struct RefScene {
    int W, H;
    std::vector<RefPixel> px;
    RefScene(int w, int h, const double bg[3])
        : W(w), H(h), px((size_t)w * h) {
        for (auto& p : px) {
            p.sealed[0] = bg[0];
            p.sealed[1] = bg[1];
            p.sealed[2] = bg[2];
        }
    }
    RefPixel& at(int x, int y) { return px[(size_t)y * W + x]; }

    void addOpaque(const Geom& g) {
        RefTri t(g, W, H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            double pc[4], depth;
            if (!t.covered(x, y, pc, depth)) continue;
            if (pc[3] <= 0.0) continue;  // 不透明 PS 丢弃
            RefPixel& q = at(x, y);
            if (depth < q.sealedDepth) {
                q.sealedDepth = depth;
                q.sealed[0] = pc[0]; q.sealed[1] = pc[1]; q.sealed[2] = pc[2];
            }
        }
    }

    void addTransparent(const Geom& g) {
        RefTri t(g, W, H);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            double pc[4], depth;
            if (!t.covered(x, y, pc, depth)) continue;
            RefPixel& q = at(x, y);
            if (!(depth < q.sealedDepth)) continue;
            double alpha = pc[3];
            if (alpha <= 0.0) continue;
            if (alpha > 1.0 - 1e-6) alpha = 1.0 - 1e-6;
            double tau = -std::log1p(-alpha);
            double z = depth, z2 = z * z, z3 = z2 * z;
            q.b[0] += tau;
            q.b[1] += tau * z;
            q.b[2] += tau * z2;
            q.b[3] += tau * z3;
            for (int c = 0; c < 3; c++) {
                double col = pc[c];
                if (col < 0.0) col = 0.0; else if (col > 1.0) col = 1.0;
                q.g[c] += tau * col;
                q.g[3 + c] += tau * col * z;
            }
        }
    }
};

struct Frag { double z, tau, r, g, b; };

// 由透明几何列表直接做精确 over 合成（不经过矩模型），用于 1/2 层核对
void exactOverRGB(int W, int H, int x, int y,
                  const std::vector<Geom>& geoms,
                  const RefScene& ref, double out[3]) {
    const RefPixel& q = ref.px[(size_t)y * W + x];
    std::vector<Frag> frags;
    for (const Geom& gg : geoms) {
        RefTri t(gg, W, H);
        double pc[4], depth;
        if (!t.covered(x, y, pc, depth)) continue;
        if (!(depth < q.sealedDepth) || pc[3] <= 0.0) continue;
        double tau = -std::log1p(-(double)pc[3]);
        frags.push_back({ depth, tau,
            std::min(1.0, std::max(0.0, pc[0])),
            std::min(1.0, std::max(0.0, pc[1])),
            std::min(1.0, std::max(0.0, pc[2])) });
    }
    // 同深度合并（tau 加权），再按深度排序
    std::vector<Frag> layers;
    for (const Frag& f : frags) {
        bool merged = false;
        for (Frag& l : layers) {
            if (std::fabs(l.z - f.z) <= 1e-12) {
                double w = l.tau + f.tau;
                l.r = (l.r * l.tau + f.r * f.tau) / w;
                l.g = (l.g * l.tau + f.g * f.tau) / w;
                l.b = (l.b * l.tau + f.b * f.tau) / w;
                l.tau = w;
                merged = true; break;
            }
        }
        if (!merged) layers.push_back(f);
    }
    for (size_t i = 0; i < layers.size(); i++)
        for (size_t j = i + 1; j < layers.size(); j++)
            if (layers[j].z < layers[i].z) std::swap(layers[i], layers[j]);
    double acc[3] = { q.sealed[0], q.sealed[1], q.sealed[2] };
    for (int k = (int)layers.size() - 1; k >= 0; k--) {
        double t = std::exp(-layers[k].tau);
        double a = -std::expm1(-layers[k].tau);
        acc[0] = layers[k].r * a + acc[0] * t;
        acc[1] = layers[k].g * a + acc[1] * t;
        acc[2] = layers[k].b * a + acc[2] * t;
    }
    out[0] = acc[0]; out[1] = acc[1]; out[2] = acc[2];
}

bool nearSeq(const double* a, const double* b, int n, double tol) {
    for (int i = 0; i < n; i++) {
        if (std::fabs(a[i] - b[i]) > tol) return false;
    }
    return true;
}

void checkMoments(const MomentOITFrame& f, const RefScene& ref,
                  int x, int y, double tol, bool& ok) {
    double m[10];
    f.GetPixelMoments(x, y, m);
    const RefPixel& q = ref.px[(size_t)y * ref.W + x];
    ok = ok && nearSeq(m, q.b, 4, tol) && nearSeq(m + 4, q.g, 6, tol);
}

void require(bool& ok, bool cond) { ok = ok && cond; }

} // namespace


//=====================================================================
// capture 组：真实 DrawPrimitive 的矩、颜色与计数
//=====================================================================

static int capture_single_layer() {
    bool ok = true;
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    double bg[3] = { 0.125, 0.25, 0.5 };
    frame.BeginFrame(bg[0], bg[1], bg[2]);
    int vsc = 0, psc = 0;
    Vec4f col(1.0f, 0.5f, 0.25f, 0.5f);
    Geom g = fullscreenConst(0.25f, col);
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_OPAQUE);
    // 无任何不透明面，直接封存背景（封存深度保持 +inf）
    frame.SealOpaque();
    install(rh, g, vsc, psc);
    TASK_CHECK(rh.DrawPrimitive());
    TASK_CHECK(vsc == 3);                 // 每次提交三个角各一次 VS
    TASK_CHECK(psc == 64);                // 覆盖整块 8x8

    RefScene ref(8, 8, bg);
    ref.addTransparent(g);
    checkMoments(frame, ref, 0, 0, PI_TOL, ok);
    checkMoments(frame, ref, 7, 7, PI_TOL, ok);
    checkMoments(frame, ref, 4, 3, PI_TOL, ok);
    TASK_CHECK(ok);

    MomentOITResult res = frame.Resolve();
    double expect[3];
    exactOverRGB(8, 8, 4, 3, { g }, ref, expect);
    TASK_CHECK(nearSeq(res.GetRGB(4, 3), expect, 3, PI_TOL));
    TASK_CHECK(res.GetLayerCount(4, 3) == 1);
    const MomentOITLayer& L = res.GetLayer(4, 3, 0);
    TASK_CHECK(std::fabs(L.depth - 0.25) <= PI_TOL);
    TASK_CHECK(std::fabs(L.alpha - 0.5) <= PI_TOL);
    TASK_CHECK(std::fabs(L.rgb[0] - 1.0) <= PI_TOL);
    TASK_CHECK(std::fabs(L.rgb_raw[1] - 0.5) <= PI_TOL);
    TASK_CHECK(std::isfinite(res.GetTransmittance(4, 3)));
    TASK_CHECK(std::fabs(res.GetTransmittance(4, 3) - 0.5) <= PI_TOL);
    frame.Cancel();
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_IDLE);
    return 0;
}

static int capture_two_layers() {
    bool ok = true;
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    double bg[3] = { 0.0, 0.0, 0.0 };
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    Geom nearG = fullscreenConst(0.25f, Vec4f(1.0f, 0.25f, 0.5f, 0.5f));
    Geom farG  = fullscreenConst(0.625f, Vec4f(0.25f, 1.0f, 0.75f, 0.25f));
    install(rh, nearG, vsc, psc);
    frame.SealOpaque();
    rh.DrawPrimitive();
    install(rh, farG, vsc, psc);
    rh.DrawPrimitive();
    TASK_CHECK(vsc == 6);

    RefScene ref(8, 8, bg);
    std::vector<Geom> geoms = { nearG, farG };
    for (const Geom& g : geoms) ref.addTransparent(g);
    checkMoments(frame, ref, 2, 2, PI_TOL, ok);
    checkMoments(frame, ref, 7, 7, PI_TOL, ok);
    TASK_CHECK(ok);

    MomentOITResult res = frame.Resolve();
    TASK_CHECK(res.GetLayerCount(3, 3) == 2);
    const MomentOITLayer& l0 = res.GetLayer(3, 3, 0);
    const MomentOITLayer& l1 = res.GetLayer(3, 3, 1);
    TASK_CHECK(l0.depth <= l1.depth);
    TASK_CHECK(std::fabs(l0.depth - 0.25) <= PI_TOL);
    TASK_CHECK(std::fabs(l1.depth - 0.625) <= PI_TOL);
    TASK_CHECK(l0.weight > 0 && l1.weight > 0);
    TASK_CHECK(std::fabs(l0.rgb[0] - 1.0) <= PI_TOL);
    TASK_CHECK(std::fabs(l1.rgb[1] - 1.0) <= PI_TOL);
    double expect[3];
    exactOverRGB(8, 8, 3, 3, geoms, ref, expect);
    TASK_CHECK(nearSeq(res.GetRGB(3, 3), expect, 3, PI_TOL));
    return 0;
}

static int capture_same_depth_mix() {
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    Geom g1 = fullscreenConst(0.5f, Vec4f(1.0f, 0.0f, 0.0f, 0.5f));
    Geom g2 = fullscreenConst(0.5f, Vec4f(0.0f, 1.0f, 0.0f, 0.25f));
    install(rh, g1, vsc, psc);
    frame.SealOpaque();
    rh.DrawPrimitive();
    install(rh, g2, vsc, psc);
    rh.DrawPrimitive();

    double tau1 = -std::log1p(-0.5);
    double tau2 = -std::log1p(-0.25);
    double m[10];
    frame.GetPixelMoments(4, 4, m);
    TASK_CHECK(std::fabs(m[0] - (tau1 + tau2)) <= 1e-12);
    TASK_CHECK(std::fabs(m[1] - (tau1 + tau2) * 0.5) <= 1e-12);

    MomentOITResult res = frame.Resolve();
    TASK_CHECK(res.GetLayerCount(4, 4) == 1);  // 同深度合并为一层
    const MomentOITLayer& L = res.GetLayer(4, 4, 0);
    double expectR = tau1 / (tau1 + tau2);
    double expectG = tau2 / (tau1 + tau2);
    TASK_CHECK(std::fabs(L.depth - 0.5) <= PI_TOL);
    TASK_CHECK(std::fabs(L.rgb[0] - expectR) <= PI_TOL);
    TASK_CHECK(std::fabs(L.rgb[1] - expectG) <= PI_TOL);
    TASK_CHECK(std::fabs(L.rgb[2] - 0.0) <= PI_TOL);
    TASK_CHECK(std::fabs(L.weight - (tau1 + tau2)) <= PI_TOL);

    // 与精确 over 合成等价
    double bg[3] = { 0, 0, 0 };
    RefScene ref(8, 8, bg);
    ref.addTransparent(g1);
    ref.addTransparent(g2);
    double expect[3];
    exactOverRGB(8, 8, 4, 4, { g1, g2 }, ref, expect);
    TASK_CHECK(nearSeq(res.GetRGB(4, 4), expect, 3, PI_TOL));
    return 0;
}

static int capture_crossing_order() {
    bool ok = true;
    const int W = 8, H = 8;
    // 两片在屏幕左右互换远近：T1 左近右远，T2 左远右近
    float d1[3] = { 0.2f, 0.8f, 0.2f };
    float d2[3] = { 0.8f, 0.2f, 0.8f };
    Geom t1 = fullscreen(d1, Vec4f(1, 0, 0, .5f),
                             Vec4f(1, 0, 0, .5f),
                             Vec4f(1, 0, 0, .5f));
    Geom t2 = fullscreen(d2, Vec4f(0, 1, 0, .5f),
                             Vec4f(0, 1, 0, .5f),
                             Vec4f(0, 1, 0, .5f));

    auto runOnce = [&](bool reversed) {
        RenderHelp rh(W, H);
        MomentOITFrame frame(rh);
        frame.BeginFrame(0.1, 0.1, 0.1);
        int vsc = 0, psc = 0;
        frame.SealOpaque();
        if (!reversed) {
            install(rh, t1, vsc, psc); rh.DrawPrimitive();
            install(rh, t2, vsc, psc); rh.DrawPrimitive();
        } else {
            install(rh, t2, vsc, psc); rh.DrawPrimitive();
            install(rh, t1, vsc, psc); rh.DrawPrimitive();
        }
        return frame.Resolve();
    };
    MomentOITResult a = runOnce(false);
    MomentOITResult b = runOnce(true);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        size_t p = (size_t)y * W + x;
        require(ok, nearSeq(&a.rgb[p * 3], &b.rgb[p * 3], 3, ORDER_TOL));
        require(ok, std::fabs(a.transmittance[p] - b.transmittance[p])
                    <= ORDER_TOL);
    }
    TASK_CHECK(ok);

    // 左右两侧的近节点深度必须对应正确的那一片
    RefTri r1(t1, W, H), r2(t2, W, H);
    for (int x : { 0, 7 }) {
        double pc[4], dep1, dep2, nx, ny, lam[3];
        r1.bary(x, 3, lam, pc, dep1, nx, ny);
        r2.bary(x, 3, lam, pc, dep2, nx, ny);
        double nearDepth = std::min(dep1, dep2);
        TASK_CHECK(a.GetLayerCount(x, 3) == 2);
        TASK_CHECK(std::fabs(a.GetLayer(x, 3, 0).depth - nearDepth) <= 1e-6);
    }
    return 0;
}

static int capture_opaque_occlusion() {
    bool ok = true;
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    double bg[3] = { 0.1, 0.1, 0.1 };
    frame.BeginFrame(bg[0], bg[1], bg[2]);
    int vsc = 0, psc = 0;
    Geom opq = fullscreenConst(0.5f, Vec4f(0.5f, 0.5f, 0.5f, 1.0f));
    install(rh, opq, vsc, psc);
    rh.DrawPrimitive();
    TASK_CHECK(vsc == 3);

    // 左侧的局部不透明面，右侧留背景
    Geom half;
    half.p[0] = Vec4f(-1, -1, 0.25f, 1);
    half.p[1] = Vec4f(0, -1, 0.25f, 1);
    half.p[2] = Vec4f(-1, 1, 0.25f, 1);
    for (int i = 0; i < 3; i++) half.c[i] = Vec4f(0.75f, 0.75f, 0.75f, 1);
    install(rh, half, vsc, psc);
    rh.DrawPrimitive();
    frame.SealOpaque();
    // 半屏三角形覆盖 sx 满足 sy >= 2*sx 的一侧：(1,4) 在内，(5,4) 在外
    TASK_CHECK(std::fabs(frame.GetSealedDepth(1, 4) - 0.25) <= 1e-7);
    TASK_CHECK(std::fabs(frame.GetSealedDepth(5, 4) - 0.5) <= 1e-7);

    Geom behind = fullscreenConst(0.75f, Vec4f(1, 0, 0, .5f));
    Geom front  = fullscreenConst(0.20f, Vec4f(0, 1, 0, .5f));
    install(rh, behind, vsc, psc); rh.DrawPrimitive();
    install(rh, front, vsc, psc);  rh.DrawPrimitive();

    RefScene refAll(8, 8, bg);
    refAll.addOpaque(opq);
    refAll.addOpaque(half);
    refAll.addTransparent(behind);   // 参考实现里它应当无处贡献
    refAll.addTransparent(front);
    RefScene refFront(8, 8, bg);
    refFront.addOpaque(opq);
    refFront.addOpaque(half);
    refFront.addTransparent(front);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        size_t p = (size_t)y * 8 + x;
        // behind 必须被完全挡住：两条参考路径的矩完全相同
        require(ok, nearSeq(refAll.px[p].b, refFront.px[p].b, 4, 0.0));
        require(ok, nearSeq(refAll.px[p].g, refFront.px[p].g, 6, 0.0));
    }
    checkMoments(frame, refFront, 5, 5, PI_TOL, ok);
    checkMoments(frame, refFront, 1, 1, PI_TOL, ok);
    TASK_CHECK(ok);

    MomentOITResult res = frame.Resolve();
    double expect[3];
    exactOverRGB(8, 8, 5, 5, { behind, front }, refFront, expect);
    TASK_CHECK(nearSeq(res.GetRGB(5, 5), expect, 3, PI_TOL));
    return 0;
}

static int capture_alpha_zero_and_tiny() {
    bool ok = true;
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0.2, 0.2, 0.2);
    int vsc = 0, psc = 0;
    frame.SealOpaque();
    Geom zero = fullscreenConst(0.3f, Vec4f(1, 0, 0, 0.0f));
    Geom tiny = fullscreenConst(0.4f, Vec4f(0, 1, 0, 1e-9f));
    install(rh, zero, vsc, psc); rh.DrawPrimitive();
    install(rh, tiny, vsc, psc); rh.DrawPrimitive();

    double m[10];
    frame.GetPixelMoments(4, 4, m);
    double tauTiny = -std::log1p(-(double)1e-9f);
    // 零 alpha 完全不贡献，只有真实的极薄层
    TASK_CHECK(std::fabs(m[0] - tauTiny) <= 1e-20);
    TASK_CHECK(std::fabs(m[1] - tauTiny * 0.4) <= 1e-16);
    for (int k = 2; k < 10; k++) require(ok, std::isfinite(m[k]));
    TASK_CHECK(ok);

    MomentOITResult res = frame.Resolve();
    TASK_CHECK(res.GetLayerCount(4, 4) == 1);  // 真实薄层未被门槛删掉
    TASK_CHECK(std::isfinite(res.GetRGB(4, 4)[0]));
    TASK_CHECK(std::fabs(res.GetTransmittance(4, 4) - std::exp(-tauTiny))
               <= 1e-16);
    TASK_CHECK(std::fabs(res.GetRGB(4, 4)[0] - 0.2) <= 1e-9);
    return 0;
}

static int capture_pierce_clip_and_w() {
    bool ok = true;
    const int W = 12, H = 12;
    double bg[3] = { 0, 0, 0 };
    RenderHelp rh(W, H);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    frame.SealOpaque();

    // 穿过左右裁剪面的侧面三角形
    Geom side;
    side.p[0] = Vec4f(-2.0f, -0.5f, 0.3f, 1);
    side.p[1] = Vec4f(1.5f, -0.5f, 0.35f, 1);
    side.p[2] = Vec4f(-2.0f, 0.8f, 0.4f, 1);
    for (int i = 0; i < 3; i++) side.c[i] = Vec4f(1, .5f, .25f, .5f);
    install(rh, side, vsc, psc);
    rh.DrawPrimitive();
    int sideCount = psc;

    // 穿过近/远裁剪面
    Geom nearfar;
    nearfar.p[0] = Vec4f(-0.8f, -0.8f, -0.5f, 1);
    nearfar.p[1] = Vec4f(0.8f, -0.8f, 1.5f, 1);
    nearfar.p[2] = Vec4f(0.0f, 0.8f, 0.3f, 1);
    for (int i = 0; i < 3; i++) nearfar.c[i] = Vec4f(.25f, .5f, .75f, .25f);
    install(rh, nearfar, vsc, psc);
    rh.DrawPrimitive();
    int nearFarCount = psc - sideCount;

    // 不同 w 的三角形，属性按透视权重恢复
    Geom persp;
    float ndc[3][3] = { {-0.8f,-0.8f,0.25f}, {0.8f,-0.8f,0.5f},
                        {0.0f,0.8f,0.75f} };
    float ws[3] = { 1.0f, 2.0f, 3.0f };
    for (int i = 0; i < 3; i++) {
        persp.p[i] = Vec4f(ndc[i][0] * ws[i], ndc[i][1] * ws[i],
                           ndc[i][2] * ws[i], ws[i]);
        persp.c[i] = Vec4f(0.25f * (i + 1), 0.5f, 0.75f, .5f);
    }
    install(rh, persp, vsc, psc);
    rh.DrawPrimitive();

    // 独立统计覆盖像素数（含完整裁剪体过滤）
    int expectSide = 0, expectNF = 0;
    RefTri rs(side, W, H), rn(nearfar, W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double pc[4], depth;
        if (rs.covered(x, y, pc, depth)) expectSide++;
        if (rn.covered(x, y, pc, depth)) expectNF++;
    }
    TASK_CHECK(sideCount == expectSide);
    TASK_CHECK(nearFarCount == expectNF);
    TASK_CHECK(sideCount > 0 && nearFarCount > 0);

    // 矩与独立参考逐像素一致
    RefScene ref(W, H, bg);
    ref.addTransparent(side);
    ref.addTransparent(nearfar);
    ref.addTransparent(persp);
    checkMoments(frame, ref, 2, 2, PI_TOL, ok);
    checkMoments(frame, ref, 9, 9, PI_TOL, ok);
    checkMoments(frame, ref, 6, 6, PI_TOL, ok);
    TASK_CHECK(ok);

    // 透视属性恢复：单独开一帧只放 persp，保证是单层，重建颜色/深度
    // 必须等于该像素独立的透视插值结果
    {
        RenderHelp rhp(W, H);
        MomentOITFrame fp(rhp);
        fp.BeginFrame(0, 0, 0);
        int vp = 0, pp = 0;
        fp.SealOpaque();
        install(rhp, persp, vp, pp);
        TASK_CHECK(rhp.DrawPrimitive());
        TASK_CHECK(vp == 3);
        MomentOITResult rp_res = fp.Resolve();
        RefTri rp(persp, W, H);
        double rpc[4], rdepth;
        TASK_CHECK(rp.covered(6, 6, rpc, rdepth));
        TASK_CHECK(rp_res.GetLayerCount(6, 6) == 1);
        const MomentOITLayer& L = rp_res.GetLayer(6, 6, 0);
        TASK_CHECK(std::fabs(L.depth - rdepth) <= PI_TOL);
        TASK_CHECK(std::fabs(L.rgb[0] -
            std::min(1.0, std::max(0.0, rpc[0]))) <= PI_TOL);
        TASK_CHECK(std::fabs(L.rgb[1] - rpc[1]) <= PI_TOL);
        TASK_CHECK(std::fabs(L.rgb[2] - rpc[2]) <= PI_TOL);
    }
    return 0;
}

static int capture_clip_fan_shared_edge() {
    bool ok = true;
    const int W = 10, H = 10;

    // ---- 第一帧：同时穿出多个裁剪面的大三角形，裁后顶点数 > 3 ----
    RenderHelp rh(W, H);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    frame.SealOpaque();

    // 顶点穿出多个裁剪面，裁后顶点数 > 3（坐标避开像素中心精确对齐）
    Geom big;
    big.p[0] = Vec4f(-2.0007f, -2.0003f, -0.5007f, 1);
    big.p[1] = Vec4f(3.00011f, -2.00019f, 1.5007f, 1);
    big.p[2] = Vec4f(-2.00023f, 3.00029f, 0.5003f, 1);
    for (int i = 0; i < 3; i++) big.c[i] = Vec4f(.5f, .5f, .5f, .5f);
    install(rh, big, vsc, psc);
    rh.DrawPrimitive();
    TASK_CHECK(psc > 0 && psc < W * H);

    // 半开覆盖与独立参考沿裁剪面边缘本就不同，这里用三条结构性断言：
    // 1) 生产端覆盖的像素必在完整裁剪体内（绝不越界）
    // 2) 严格内部像素必须被覆盖（fan 无洞），且光学厚度恰为单个 tau，
    //    即 fan 内部边不会重复累加
    // 3) 投影三角形内但落在裁剪体外的像素必须被裁掉
    RefTri rb(big, W, H);
    int prodCovered = 0, interiorSeen = 0, outsideSeen = 0;
    // 独立参考用 double 重算透视插值；生产端 varying 走 float，alpha 有
    // ~3e-8 的舍入，故按任务精度门槛 1e-7 比对。fan 若重复累加，b0 会差
    // 约一个 tau（~0.69），远大于该门槛，仍能可靠检出。
    double bg0[3] = { 0, 0, 0 };
    RefScene refBig(W, H, bg0);
    refBig.addTransparent(big);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double m[10];
        frame.GetPixelMoments(x, y, m);
        bool prodHit = m[0] > 0.0;
        if (prodHit) prodCovered++;

        double lam[3], rpc[4], rdep, rnx, rny;
        bool inProjected = rb.bary(x, y, lam, rpc, rdep, rnx, rny);
        bool inClip = inProjected &&
            rnx >= -1.0 && rnx <= 1.0 && rny >= -1.0 && rny <= 1.0 &&
            rdep >= 0.0 && rdep <= 1.0;
        const double margin = 0.02;
        bool interior = inProjected &&
            rnx > -1.0 + margin && rnx < 1.0 - margin &&
            rny > -1.0 + margin && rny < 1.0 - margin &&
            rdep > margin && rdep < 1.0 - margin;
        if (inProjected && !inClip) outsideSeen++;

        if (prodHit) require(ok, inClip);              // 断言 1
        if (interior) {
            interiorSeen++;
            require(ok, prodHit);                      // 断言 2：fan 无洞
            // 内部像素逐矩比对参考（含 fan 内部边不重复累加）
            checkMoments(frame, refBig, x, y, PI_TOL, ok);
        }
    }
    TASK_CHECK(ok);
    TASK_CHECK(prodCovered == psc);   // 每个覆盖像素恰好执行一次 PS
    TASK_CHECK(interiorSeen > 10);
    TASK_CHECK(outsideSeen > 0);      // 该三角形确实有被裁掉的部分
    frame.Cancel();

    // ---- 第二帧：相邻两三角形拼成整屏四边形，验证共边半开覆盖 ----
    RenderHelp rh2(W, H);
    MomentOITFrame f2(rh2);
    f2.BeginFrame(0, 0, 0);
    int v2 = 0, p2 = 0;
    f2.SealOpaque();
    Geom q1, q2;
    q1.p[0] = Vec4f(-1, -1, .3f, 1);
    q1.p[1] = Vec4f(1, -1, .3f, 1);
    q1.p[2] = Vec4f(-1, 1, .3f, 1);
    q2.p[0] = Vec4f(1, -1, .3f, 1);
    q2.p[1] = Vec4f(1, 1, .3f, 1);
    q2.p[2] = Vec4f(-1, 1, .3f, 1);
    for (int i = 0; i < 3; i++) {
        q1.c[i] = q2.c[i] = Vec4f(.25f, .5f, .75f, .5f);
    }
    install(rh2, q1, v2, p2); rh2.DrawPrimitive();
    install(rh2, q2, v2, p2); rh2.DrawPrimitive();
    TASK_CHECK(p2 == W * H);       // 共边半开覆盖：每像素恰好一次

    // 四边形颜色/深度在三个角上为常量；float 透视插值在贴近边的像素上有
    // ~1e-8 舍入，按任务门槛 1e-7 比对。若 fan/共边重复累加，b0 会变成
    // 约 2*tau（~1.386），与单 tau（~0.693）相差 0.69，远大于该门槛。
    double tauQ = -std::log1p(-0.5);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        double m[10];
        f2.GetPixelMoments(x, y, m);
        // 内部边上 alpha 不能被加两次：每像素光学厚度恰为一个 tauQ
        require(ok, std::fabs(m[0] - tauQ) <= PI_TOL);
        require(ok, std::fabs(m[1] - tauQ * 0.3) <= PI_TOL);
        require(ok, std::isfinite(m[3]));
    }
    TASK_CHECK(ok);
    return 0;
}


//=====================================================================
// reconstruct 组：求解器、精度与近似边界
//=====================================================================

static int reconstruct_three_layers() {
    bool ok = true;
    const int N = 8;
    RenderHelp rh(N, N);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0.1, 0.1, 0.1);
    int vsc = 0, psc = 0;
    Geom g0 = fullscreenConst(0.125f, Vec4f(1, .25f, .5f, .5f));
    Geom g1 = fullscreenConst(0.375f, Vec4f(.25f, 1, .5f, .4f));
    Geom g2 = fullscreenConst(0.75f,  Vec4f(.5f, .25f, 1, .3f));
    std::vector<Geom> geoms = { g0, g1, g2 };
    frame.SealOpaque();
    for (Geom& g : geoms) { install(rh, g, vsc, psc); rh.DrawPrimitive(); }

    // 独立核对四个光学矩与六个颜色矩
    double bg[3] = { 0.1, 0.1, 0.1 };
    RefScene ref(N, N, bg);
    for (const Geom& g : geoms) ref.addTransparent(g);
    checkMoments(frame, ref, 3, 3, PI_TOL, ok);
    checkMoments(frame, ref, 7, 7, PI_TOL, ok);
    TASK_CHECK(ok);

    MomentOITResult res = frame.Resolve();
    int n = res.GetLayerCount(3, 3);
    TASK_CHECK(n >= 1 && n <= 2);
    double sumB[4] = { 0, 0, 0, 0 };
    double sumG[6] = { 0, 0, 0, 0, 0, 0 };
    for (int k = 0; k < n; k++) {
        const MomentOITLayer& L = res.GetLayer(3, 3, k);
        TASK_CHECK(std::isfinite(L.depth) && L.depth >= 0 && L.depth <= 1);
        TASK_CHECK(L.weight > 0.0 && std::isfinite(L.weight));
        TASK_CHECK(L.rgb[0] >= 0 && L.rgb[0] <= 1);
        double d = L.depth, w = L.weight;
        sumB[0] += w; sumB[1] += w * d;
        sumB[2] += w * d * d; sumB[3] += w * d * d * d;
        for (int c = 0; c < 3; c++) {
            sumG[c] += w * L.rgb[c];
            sumG[3 + c] += w * L.rgb[c] * d;
        }
    }
    double m[10];
    frame.GetPixelMoments(3, 3, m);
    TASK_CHECK(nearSeq(sumB, m, 4, 1e-7));       // 三层：矩误差 ≤1e-7
    TASK_CHECK(nearSeq(sumG, m + 4, 6, 1e-7));
    return 0;
}

static int reconstruct_near_merge() {
    // 间隔小于 1e-4 的近合并：允许退化为单节点，但不能出 NaN/负质量
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    frame.SealOpaque();
    Geom a = fullscreenConst(0.25f, Vec4f(1, .5f, .25f, .4f));
    Geom b = fullscreenConst(0.25003f, Vec4f(.25f, .5f, 1, .3f));
    install(rh, a, vsc, psc); rh.DrawPrimitive();
    install(rh, b, vsc, psc); rh.DrawPrimitive();
    MomentOITResult res = frame.Resolve();
    int n = res.GetLayerCount(4, 4);
    TASK_CHECK(n >= 1 && n <= 2);
    for (int k = 0; k < n; k++) {
        const MomentOITLayer& L = res.GetLayer(4, 4, k);
        TASK_CHECK(std::isfinite(L.depth) && std::isfinite(L.weight));
        TASK_CHECK(L.weight > 0.0);
        TASK_CHECK(L.depth >= 0.0 && L.depth <= 1.0);
        for (int c = 0; c < 3; c++)
            TASK_CHECK(std::isfinite(L.rgb[c]) && std::isfinite(L.rgb_raw[c]));
    }
    TASK_CHECK(std::isfinite(res.GetRGB(4, 4)[0]));
    TASK_CHECK(std::isfinite(res.GetTransmittance(4, 4)));

    // 间隔 1e-4 的可分辨双节点
    RenderHelp rh2(8, 8);
    MomentOITFrame f2(rh2);
    f2.BeginFrame(0, 0, 0);
    int v2 = 0, p2 = 0;
    f2.SealOpaque();
    Geom c1 = fullscreenConst(0.25f, Vec4f(1, .5f, .25f, .4f));
    Geom c2 = fullscreenConst(0.2501f, Vec4f(.25f, .5f, 1, .3f));
    install(rh2, c1, v2, p2); rh2.DrawPrimitive();
    install(rh2, c2, v2, p2); rh2.DrawPrimitive();
    MomentOITResult r2 = f2.Resolve();
    TASK_CHECK(r2.GetLayerCount(4, 4) == 2);
    TASK_CHECK(std::fabs(r2.GetLayer(4, 4, 0).depth - 0.25) <= 1e-6);
    TASK_CHECK(std::fabs(r2.GetLayer(4, 4, 1).depth - 0.2501) <= 1e-6);
    return 0;
}

static int reconstruct_empty_frame() {
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0.3, 0.4, 0.5);
    frame.SealOpaque();
    MomentOITResult res = frame.Resolve();
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        TASK_CHECK(res.GetLayerCount(x, y) == 0);
        TASK_CHECK(res.GetRGB(x, y)[0] == 0.3);
        TASK_CHECK(res.GetRGB(x, y)[1] == 0.4);
        TASK_CHECK(res.GetRGB(x, y)[2] == 0.5);
        TASK_CHECK(res.GetTransmittance(x, y) == 1.0);
    }
    return 0;
}

static int reconstruct_reverse_order() {
    bool ok = true;
    auto run = [&](bool rev) {
        RenderHelp rh(8, 8);
        MomentOITFrame frame(rh);
        frame.BeginFrame(0.1, 0.2, 0.3);
        int vsc = 0, psc = 0;
        frame.SealOpaque();
        Geom g1 = fullscreenConst(0.25f, Vec4f(1, .25f, .5f, .5f));
        Geom g2 = fullscreenConst(0.625f, Vec4f(.5f, 1, .25f, .35f));
        if (!rev) {
            install(rh, g1, vsc, psc); rh.DrawPrimitive();
            install(rh, g2, vsc, psc); rh.DrawPrimitive();
        } else {
            install(rh, g2, vsc, psc); rh.DrawPrimitive();
            install(rh, g1, vsc, psc); rh.DrawPrimitive();
        }
        return frame.Resolve();
    };
    MomentOITResult a = run(false), b = run(true);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        size_t p = (size_t)y * 8 + x;
        require(ok, nearSeq(&a.rgb[p * 3], &b.rgb[p * 3], 3, ORDER_TOL));
        require(ok, std::fabs(a.transmittance[p] - b.transmittance[p])
                    <= ORDER_TOL);
        require(ok, a.layer_count[p] == b.layer_count[p]);
    }
    TASK_CHECK(ok);
    return 0;
}

static int reconstruct_high_opacity() {
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    frame.SealOpaque();
    float hi = 1.0f - 1e-6f;
    Geom g1 = fullscreenConst(0.25f, Vec4f(1, 1, 1, hi));
    Geom g2 = fullscreenConst(0.5f, Vec4f(1, 0, 0, hi));
    install(rh, g1, vsc, psc); rh.DrawPrimitive();
    install(rh, g2, vsc, psc); rh.DrawPrimitive();
    MomentOITResult res = frame.Resolve();
    TASK_CHECK(res.GetLayerCount(4, 4) == 2);
    for (int k = 0; k < 2; k++) {
        const MomentOITLayer& L = res.GetLayer(4, 4, k);
        TASK_CHECK(std::isfinite(L.weight) && L.weight > 0);
        TASK_CHECK(L.alpha > 0 && L.alpha <= 1.0f);
        TASK_CHECK(L.transmittance > 0.0 && L.transmittance <= 1.0);
    }
    TASK_CHECK(res.GetTransmittance(4, 4) > 0.0);
    TASK_CHECK(std::isfinite(res.GetRGB(4, 4)[0]));
    return 0;
}

static int reconstruct_raw_and_clamp() {
    bool ok = true;
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    frame.SealOpaque();
    Geom g1 = fullscreenConst(0.25f, Vec4f(1, .5f, .25f, .5f));
    Geom g2 = fullscreenConst(0.625f, Vec4f(.25f, .5f, 1, .25f));
    install(rh, g1, vsc, psc); rh.DrawPrimitive();
    install(rh, g2, vsc, psc); rh.DrawPrimitive();
    MomentOITResult res = frame.Resolve();
    // 两层精确情形：未钳颜色即真实片元色
    const MomentOITLayer& l0 = res.GetLayer(4, 4, 0);
    const MomentOITLayer& l1 = res.GetLayer(4, 4, 1);
    TASK_CHECK(nearSeq(l0.rgb_raw, l0.rgb, 3, 1e-7));
    TASK_CHECK(std::fabs(l0.rgb_raw[0] - 1.0) <= 1e-7);
    TASK_CHECK(std::fabs(l1.rgb_raw[2] - 1.0) <= 1e-7);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        for (int k = 0; k < res.GetLayerCount(x, y); k++)
            for (int c = 0; c < 3; c++) {
                require(ok, res.GetLayer(x, y, k).rgb[c] >= 0.0);
                require(ok, res.GetLayer(x, y, k).rgb[c] <= 1.0);
            }
    TASK_CHECK(ok);
    return 0;
}


//=====================================================================
// lifecycle 组：阶段机、快照独立性与设备交错
//=====================================================================

static int lifecycle_phase_transitions() {
    RenderHelp rh(8, 8);
    MomentOITFrame frame(rh);
    auto throwsLogic = [&](auto&& fn) {
        try { fn(); }
        catch (const std::logic_error&) { return true; }
        catch (...) { return false; }
        return false;
    };
    TASK_CHECK(throwsLogic([&] { frame.SealOpaque(); }));
    TASK_CHECK(throwsLogic([&] { frame.Resolve(); }));
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_IDLE);

    frame.BeginFrame(0, 0, 0);
    TASK_CHECK(throwsLogic([&] { frame.BeginFrame(0, 0, 0); }));
    TASK_CHECK(throwsLogic([&] { frame.Resolve(); }));   // 没封存不能解析
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_OPAQUE);

    frame.SealOpaque();
    TASK_CHECK(throwsLogic([&] { frame.SealOpaque(); }));
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_TRANSPARENT);

    // 异常之后合法流程仍可用
    int vsc = 0, psc = 0;
    Geom g = fullscreenConst(0.3f, Vec4f(1, 1, 1, .5f));
    install(rh, g, vsc, psc);
    TASK_CHECK(rh.DrawPrimitive());
    TASK_CHECK(frame.Resolve().GetLayerCount(4, 4) == 1);

    frame.Cancel();
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_IDLE);
    frame.Cancel();   // 空闲取消合法
    // 取消后可以重新开始
    frame.BeginFrame(0.2, 0.2, 0.2);
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_OPAQUE);
    frame.Cancel();
    return 0;
}

struct Probe : RenderHelp {
    using RenderHelp::RenderHelp;
    uint32_t pixel(int x, int y) { return _frame_buffer->GetPixel(x, y); }
    float depth(int x, int y) { return _depth_buffer[y][x]; }
};

static int lifecycle_resolve_readonly() {
    Probe rh(8, 8);
    MomentOITFrame frame(rh);
    uint32_t beforePix = rh.pixel(4, 4);
    float beforeDepth = rh.depth(4, 4);

    frame.BeginFrame(0.1, 0.2, 0.3);
    int vsc = 0, psc = 0;
    Geom opq = fullscreenConst(0.5f, Vec4f(.5f, .5f, .5f, 1));
    install(rh, opq, vsc, psc);
    rh.DrawPrimitive();
    frame.SealOpaque();
    Geom tr = fullscreenConst(0.25f, Vec4f(1, .5f, .25f, .5f));
    install(rh, tr, vsc, psc);
    rh.DrawPrimitive();
    int vsAfterCapture = vsc, psAfterCapture = psc;

    MomentOITResult r1 = frame.Resolve();
    MomentOITResult r2 = frame.Resolve();   // 允许多次解析
    TASK_CHECK(vsc == vsAfterCapture);       // 解析不执行 VS/PS
    TASK_CHECK(psc == psAfterCapture);
    TASK_CHECK(r1.rgb == r2.rgb);
    TASK_CHECK(r1.transmittance == r2.transmittance);
    TASK_CHECK(r1.layer_count == r2.layer_count);

    // 不写普通帧的颜色和深度，封存后透明面也不占深度
    TASK_CHECK(rh.pixel(4, 4) == beforePix);
    TASK_CHECK(rh.depth(4, 4) == beforeDepth);
    TASK_CHECK(rh.depth(0, 0) == 0.0f);
    return 0;
}

static int lifecycle_two_devices() {
    bool ok = true;
    Probe d1(8, 8), d2(6, 6);
    MomentOITFrame f1(d1), f2(d2);
    f1.BeginFrame(0.1, 0.1, 0.1);
    f2.BeginFrame(0.2, 0.2, 0.2);
    int c1 = 0, p1 = 0, c2 = 0, p2 = 0;
    Geom g1 = fullscreenConst(0.25f, Vec4f(1, 0, 0, .5f));
    install(d1, g1, c1, p1);
    TASK_CHECK(d1.DrawPrimitive());
    Geom g2 = fullscreenConst(0.5f, Vec4f(0, 0, 1, .25f));
    install(d2, g2, c2, p2);
    TASK_CHECK(d2.DrawPrimitive());
    f1.SealOpaque();
    f2.SealOpaque();
    Geom t1 = fullscreenConst(0.125f, Vec4f(0, 1, 0, .4f));
    install(d1, t1, c1, p1); d1.DrawPrimitive();
    Geom t2 = fullscreenConst(0.375f, Vec4f(1, 1, 0, .3f));
    install(d2, t2, c2, p2); d2.DrawPrimitive();

    MomentOITResult r1 = f1.Resolve();
    MomentOITResult r2 = f2.Resolve();
    TASK_CHECK(r1.width == 8 && r1.height == 8);
    TASK_CHECK(r2.width == 6 && r2.height == 6);
    double bg1[3] = { 0.1, 0.1, 0.1 }, bg2[3] = { 0.2, 0.2, 0.2 };
    RefScene ref1(8, 8, bg1); ref1.addTransparent(t1);
    RefScene ref2(6, 6, bg2); ref2.addTransparent(t2);
    checkMoments(f1, ref1, 4, 4, PI_TOL, ok);
    double m2[10];
    f2.GetPixelMoments(3, 3, m2);
    TASK_CHECK(nearSeq(m2, ref2.px[(size_t)3 * 6 + 3].b, 4, PI_TOL));
    TASK_CHECK(ok);
    f1.Cancel(); f2.Cancel();
    return 0;
}

static int lifecycle_input_mutation_snapshot() {
    Probe rh(8, 8);
    auto positions = std::make_unique<std::vector<Vec4f>>();
    positions->push_back(Vec4f(-1, -1, .25f, 1));
    positions->push_back(Vec4f(3, -1, .25f, 1));
    positions->push_back(Vec4f(-1, 3, .25f, 1));
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    int vsc = 0, psc = 0;
    rh.SetVertexShader([&](int i, ShaderContext& o) -> Vec4f {
        ++vsc;
        o.varying_vec4f[0] = Vec4f(1, .5f, .25f, .5f);
        return (*positions)[i];
    });
    rh.SetPixelShader([&](ShaderContext& in) -> Vec4f {
        ++psc;
        return in.varying_vec4f[0];
    });
    frame.SealOpaque();
    rh.DrawPrimitive();
    MomentOITResult snapshot = frame.Resolve();

    // 修改并释放回调捕获的输入，旧结果不变
    (*positions)[0] = Vec4f(0, 0, 0.9f, 1);
    MomentOITResult again = frame.Resolve();
    TASK_CHECK(snapshot.rgb == again.rgb);
    positions.reset();
    MomentOITResult third = frame.Resolve();   // 解析不再触发 VS/PS
    TASK_CHECK(snapshot.rgb == third.rgb);
    TASK_CHECK(vsc == 3 && psc == 64);
    frame.Cancel();
    return 0;
}

static int lifecycle_clear_init_reset() {
    Probe rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    rh.Clear();
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_IDLE);
    frame.BeginFrame(0.1, 0.1, 0.1);  // 取消后可重新开始
    frame.SealOpaque();
    rh.Init(8, 8);
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_IDLE);
    frame.BeginFrame(0.2, 0.2, 0.2);
    rh.Reset();
    TASK_CHECK(frame.GetPhase() == MomentOITFrame::PHASE_IDLE);
    bool threw = false;
    try { frame.BeginFrame(0, 0, 0); }
    catch (const std::logic_error&) { threw = true; }
    TASK_CHECK(threw);  // 无帧缓存时拒绝开始
    return 0;
}

static int lifecycle_destructor_detaches() {
    Probe rh(8, 8);
    {
        MomentOITFrame frame(rh);
        frame.BeginFrame(0, 0, 0);
        frame.SealOpaque();
    }
    TASK_CHECK(!rh._momentActive());
    // 设备回到普通帧行为（普通帧不做齐次裁剪，用落在 CVV 内的三角形）
    int vsc = 0, psc = 0;
    Geom g;
    g.p[0] = Vec4f(-0.8f, -0.8f, 0.5f, 1);
    g.p[1] = Vec4f(0.8f, -0.8f, 0.5f, 1);
    g.p[2] = Vec4f(0.0f, 0.8f, 0.5f, 1);
    for (int i = 0; i < 3; i++) g.c[i] = Vec4f(1, 0, 0, 1);
    install(rh, g, vsc, psc);
    TASK_CHECK(rh.DrawPrimitive());
    TASK_CHECK(vsc == 3 && psc > 0);
    TASK_CHECK((rh.pixel(4, 4) & 0xffffff) ==
               (vector_to_color(Vec4f(1, 0, 0, 1)) & 0xffffff));
    return 0;
}

static int lifecycle_old_snapshot_independent() {
    Probe rh(8, 8);
    MomentOITFrame frame(rh);
    frame.BeginFrame(0, 0, 0);
    frame.SealOpaque();
    int vsc = 0, psc = 0;
    Geom g1 = fullscreenConst(0.25f, Vec4f(1, .5f, .25f, .5f));
    install(rh, g1, vsc, psc);
    rh.DrawPrimitive();
    MomentOITResult old = frame.Resolve();
    frame.Cancel();

    // 新帧使用完全不同的数据
    frame.BeginFrame(0.9, 0.9, 0.9);
    frame.SealOpaque();
    Geom g2 = fullscreenConst(0.75f, Vec4f(.25f, .25f, .25f, .25f));
    install(rh, g2, vsc, psc);
    rh.DrawPrimitive();
    MomentOITResult now = frame.Resolve();
    TASK_CHECK(old.rgb != now.rgb);
    TASK_CHECK(std::fabs(old.GetRGB(4, 4)[0] - now.GetRGB(4, 4)[0]) > 1e-6);
    TASK_CHECK(old.GetLayerCount(4, 4) == 1);
    frame.Cancel();
    return 0;
}


//=====================================================================
// 用例注册表
//=====================================================================
const struct task_case task_cases[] = {
    { "capture", "single_layer",          capture_single_layer },
    { "capture", "two_layers",            capture_two_layers },
    { "capture", "same_depth_mix",        capture_same_depth_mix },
    { "capture", "crossing_order_swap",   capture_crossing_order },
    { "capture", "opaque_occlusion",      capture_opaque_occlusion },
    { "capture", "alpha_zero_and_tiny",   capture_alpha_zero_and_tiny },
    { "capture", "pierce_clip_and_w",     capture_pierce_clip_and_w },
    { "capture", "clip_fan_shared_edge",  capture_clip_fan_shared_edge },

    { "reconstruct", "three_layers",          reconstruct_three_layers },
    { "reconstruct", "near_merge_nodes",      reconstruct_near_merge },
    { "reconstruct", "empty_frame",           reconstruct_empty_frame },
    { "reconstruct", "reverse_submission",    reconstruct_reverse_order },
    { "reconstruct", "high_opacity",          reconstruct_high_opacity },
    { "reconstruct", "raw_and_clamped_color", reconstruct_raw_and_clamp },

    { "lifecycle", "phase_transitions",       lifecycle_phase_transitions },
    { "lifecycle", "resolve_readonly",        lifecycle_resolve_readonly },
    { "lifecycle", "two_devices_interleaved", lifecycle_two_devices },
    { "lifecycle", "input_mutation_snapshot", lifecycle_input_mutation_snapshot },
    { "lifecycle", "clear_init_reset_cancel", lifecycle_clear_init_reset },
    { "lifecycle", "destructor_detaches",     lifecycle_destructor_detaches },
    { "lifecycle", "old_snapshot_independent",lifecycle_old_snapshot_independent },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
