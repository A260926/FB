//=====================================================================
//
// moment_oit.cpp - 矩透明离屏帧实现
//
// 光栅化（齐次裁剪、半开覆盖、透视属性遍历）复用 RenderHelp.h 中的
// 共享实现，本文件只负责帧生命周期、每像素十个 double 的矩累加，
// 以及二节点高斯求积解析。
//
//=====================================================================
#include "moment_oit.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <algorithm>


//---------------------------------------------------------------------
// 内部工具
//---------------------------------------------------------------------
namespace {

inline bool finiteDouble(double x) { return std::isfinite(x); }

inline double clamp01(double x) {
    if (!(x > 0.0)) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

} // namespace


//---------------------------------------------------------------------
// 生命周期
//---------------------------------------------------------------------
MomentOITFrame::MomentOITFrame(RenderHelp& rh)
    : _rh(rh), _phase(PHASE_IDLE), _width(0), _height(0)
{
}

MomentOITFrame::~MomentOITFrame()
{
    if (_phase != PHASE_IDLE) {
        // 不让析构抛异常：静默摘除回调
        _rh._momentDetach();
    }
}

void MomentOITFrame::requirePhase(Phase expected, const char* what) const
{
    if (_phase != expected) {
        throw std::logic_error(std::string("MomentOITFrame::") + what +
            ": invalid phase for this operation");
    }
}

void MomentOITFrame::allocateStorage()
{
    size_t pixels = (size_t)_width * (size_t)_height;
    _sealed_rgb.assign(pixels * 3, 0.0);
    _sealed_depth.assign(pixels,
        std::numeric_limits<double>::infinity());
    _moments.assign(pixels * kMomentsPerPixel, 0.0);
}

void MomentOITFrame::BeginFrame(double bg_r, double bg_g, double bg_b)
{
    // 阶段检查先于任何修改
    if (_phase != PHASE_IDLE) {
        throw std::logic_error(
            "MomentOITFrame::BeginFrame: a moment frame is already active");
    }
    if (_rh.GetFrameBuffer() == nullptr) {
        throw std::logic_error(
            "MomentOITFrame::BeginFrame: device has no frame buffer");
    }
    if (!finiteDouble(bg_r) || !finiteDouble(bg_g) || !finiteDouble(bg_b)) {
        throw std::logic_error(
            "MomentOITFrame::BeginFrame: background color must be finite");
    }

    _width = _rh.GetWidth();
    _height = _rh.GetHeight();
    allocateStorage();

    double bg[3] = { clamp01(bg_r), clamp01(bg_g), clamp01(bg_b) };
    for (size_t p = 0; p < (size_t)_width * (size_t)_height; p++) {
        _sealed_rgb[p * 3 + 0] = bg[0];
        _sealed_rgb[p * 3 + 1] = bg[1];
        _sealed_rgb[p * 3 + 2] = bg[2];
    }

    _phase = PHASE_OPAQUE;

    MomentOpaqueHook opaque_hook =
        [this](int x, int y, double depth,
               double r, double g, double b) {
            onOpaqueFragment(x, y, depth, r, g, b);
        };
    MomentTransparentHook transparent_hook =
        [this](int x, int y, double depth,
               double r, double g, double b, double alpha) {
            onTransparentFragment(x, y, depth, r, g, b, alpha);
        };
    std::function<void()> cancel_hook = [this]() {
        // 设备 Clear/Init/Reset 触发：摘除回调并回到 Idle
        if (_phase != PHASE_IDLE) {
            _phase = PHASE_IDLE;
            _rh._momentDetach();
        }
    };

    _rh._momentAttach(PHASE_OPAQUE, opaque_hook,
                      transparent_hook, cancel_hook);
}

void MomentOITFrame::SealOpaque()
{
    requirePhase(PHASE_OPAQUE, "SealOpaque");
    // 颜色与窗口深度已在不透明阶段逐像素写入封存数组，这里只切换阶段
    _phase = PHASE_TRANSPARENT;
    _rh._momentSetPhase(PHASE_TRANSPARENT);
}

void MomentOITFrame::Cancel()
{
    if (_phase == PHASE_IDLE) return;
    _phase = PHASE_IDLE;
    _rh._momentDetach();
    _sealed_rgb.clear();
    _sealed_depth.clear();
    _moments.clear();
    _width = _height = 0;
}

//---------------------------------------------------------------------
// 片元回调（由共享光栅器在矩帧期间调用）
//---------------------------------------------------------------------
void MomentOITFrame::onOpaqueFragment(int x, int y, double depth,
                                      double r, double g, double b)
{
    if (_phase != PHASE_OPAQUE) return;
    size_t p = (size_t)y * (size_t)_width + (size_t)x;
    // 严格更近才写；PS 丢弃判定在光栅器一侧完成
    if (depth < _sealed_depth[p]) {
        _sealed_depth[p] = depth;
        _sealed_rgb[p * 3 + 0] = r;
        _sealed_rgb[p * 3 + 1] = g;
        _sealed_rgb[p * 3 + 2] = b;
    }
}

void MomentOITFrame::onTransparentFragment(int x, int y, double depth,
                                           double r, double g,
                                           double b, double alpha)
{
    if (_phase != PHASE_TRANSPARENT) return;
    if (alpha <= 0.0) return;   // 不做全局门槛，零 alpha 只是这一片不贡献
    size_t p = (size_t)y * (size_t)_width + (size_t)x;
    // 严格位于封存深度之前才有贡献，透明面不占深度
    if (!(depth < _sealed_depth[p])) return;

    // tau = -log(1-alpha)，即便 alpha 极小也保留，不删除真实薄层
    double tau = -std::log1p(-alpha);
    if (!(tau > 0.0)) return;

    double z = depth;
    double z2 = z * z;
    double z3 = z2 * z;

    double* m = &_moments[p * kMomentsPerPixel];
    m[0] += tau;          // b0
    m[1] += tau * z;      // b1
    m[2] += tau * z2;     // b2
    m[3] += tau * z3;     // b3
    m[4] += tau * r;      // g0 r
    m[5] += tau * g;      // g0 g
    m[6] += tau * b;      // g0 b
    m[7] += tau * r * z;  // g1 r
    m[8] += tau * g * z;  // g1 g
    m[9] += tau * b * z;  // g1 b
}


//---------------------------------------------------------------------
// 原始矩只读访问
//---------------------------------------------------------------------
void MomentOITFrame::GetPixelMoments(int x, int y, double out_moments[10]) const
{
    requirePhase(PHASE_TRANSPARENT, "GetPixelMoments");
    if (x < 0 || y < 0 || x >= _width || y >= _height) {
        throw std::logic_error(
            "MomentOITFrame::GetPixelMoments: pixel out of range");
    }
    size_t p = (size_t)y * (size_t)_width + (size_t)x;
    for (int k = 0; k < kMomentsPerPixel; k++) {
        out_moments[k] = _moments[p * kMomentsPerPixel + k];
    }
}

double MomentOITFrame::GetSealedDepth(int x, int y) const
{
    requirePhase(PHASE_TRANSPARENT, "GetSealedDepth");
    if (x < 0 || y < 0 || x >= _width || y >= _height) {
        throw std::logic_error(
            "MomentOITFrame::GetSealedDepth: pixel out of range");
    }
    size_t p = (size_t)y * (size_t)_width + (size_t)x;
    return _sealed_depth[p];
}


//---------------------------------------------------------------------
// 二节点高斯求积：由 b0..b3 恢复深度与正光学质量
//---------------------------------------------------------------------
int MomentGaussNodes(const double b[4],
                     double& d0, double& w0,
                     double& d1, double& w1)
{
    d0 = d1 = 0.0;
    w0 = w1 = 0.0;

    // 空像素（含 b0 异常的防御性处理）
    if (!(b[0] > 0.0) || !std::isfinite(b[0])) {
        return 0;
    }

    double total = b[0];
    double m1 = b[1] / total;
    double m2 = b[2] / total;
    double m3 = b[3] / total;

    // 中心矩
    double mu2 = m2 - m1 * m1;
    double mu3 = m3 - 3.0 * m1 * m2 + 2.0 * m1 * m1 * m1;

    double scale = std::max({1.0, std::fabs(m1), std::sqrt(
        std::fabs(m2) > 0.0 ? std::fabs(m2) : 0.0)});
    double varEps = 1e-20 * scale * scale;

    if (!(mu2 > varEps) || !std::isfinite(mu2) || !std::isfinite(mu3)) {
        // 单深度退化：所有质量放在一个节点上，矩 0/1 精确，2/3 随之确定
        double d = std::isfinite(m1) ? m1 : 0.0;
        if (d < 0.0) d = 0.0;
        d0 = d;
        w0 = total;
        return 1;
    }

    // 二阶正交多项式 (x-m1)^2 - s (x-m1) - mu2，s = mu3/mu2
    double s = mu3 / mu2;
    double half = 0.5 * s;
    double disc2 = half * half + mu2;
    if (!(disc2 > 0.0) || !std::isfinite(disc2)) {
        d0 = std::max(m1, 0.0);
        w0 = total;
        return 1;
    }
    double disc = std::sqrt(disc2);
    d0 = m1 + half - disc;
    d1 = m1 + half + disc;
    if (d0 < 0.0) d0 = 0.0;
    if (!(d1 > d0)) {
        d0 = std::max(m1, 0.0);
        w0 = total;
        return 1;
    }

    // 权重由矩 0/1 联立：a0 d0 + a1 d1 = m1, a0+a1=1，
    // 故 a0 = (d1-m1)/(d1-d0)，数学上严格为正；做一次非负防御
    double a0 = (d1 - m1) / (d1 - d0);
    double a1 = 1.0 - a0;
    if (!(a0 > 0.0) || !(a1 > 0.0)) {
        d0 = std::max(m1, 0.0);
        w0 = total;
        return 1;
    }
    w0 = total * a0;
    w1 = total * a1;
    if (!(w0 > 0.0) || !(w1 > 0.0)) {
        d0 = std::max(m1, 0.0);
        w0 = total;
        d1 = 0.0;
        w1 = 0.0;
        return 1;
    }
    return 2;
}


//---------------------------------------------------------------------
// 层色恢复与由近到远合成
//---------------------------------------------------------------------
double MomentCompose(const double b[4],
                     const double g[6],
                     const double bg[3],
                     int node_count,
                     double d0, double w0, double d1, double w1,
                     MomentOITLayer out_layers[2],
                     double out_rgb[3])
{
    double total_trans = std::exp(-b[0]);

    if (node_count == 0) {
        out_rgb[0] = bg[0];
        out_rgb[1] = bg[1];
        out_rgb[2] = bg[2];
        return 1.0;
    }

    double depths[2] = { d0, d1 };
    double weights[2] = { w0, w1 };

    for (int j = 0; j < node_count; j++) {
        MomentOITLayer& layer = out_layers[j];
        layer.depth = depths[j];
        layer.weight = weights[j];
        layer.transmittance = std::exp(-weights[j]);
        layer.alpha = -std::expm1(-weights[j]);
    }

    if (node_count == 1) {
        // sum(wj Cj) = g0
        double inv = 1.0 / w0;
        for (int c = 0; c < 3; c++) {
            double raw = g[c] * inv;
            out_layers[0].rgb_raw[c] = raw;
            out_layers[0].rgb[c] = clamp01(raw);
        }
    }
    else {
        // [w0 w1; w0 d0 w1 d1] [C0; C1] = [g0; g1]，行列式 = w0 w1 (d1-d0)
        double det = w0 * w1 * (d1 - d0);
        double inv_det = 1.0 / det;
        for (int c = 0; c < 3; c++) {
            double g0c = g[c];
            double g1c = g[3 + c];
            double raw0 = (g0c * (w1 * d1) - g1c * w1) * inv_det;
            double raw1 = (g1c * w0 - g0c * (w0 * d0)) * inv_det;
            out_layers[0].rgb_raw[c] = raw0;
            out_layers[1].rgb_raw[c] = raw1;
            out_layers[0].rgb[c] = clamp01(raw0);
            out_layers[1].rgb[c] = clamp01(raw1);
        }
    }

    // 由远到近在封存颜色上合成
    double acc[3] = { bg[0], bg[1], bg[2] };
    for (int j = node_count - 1; j >= 0; j--) {
        double t = out_layers[j].transmittance;
        double a = out_layers[j].alpha;
        for (int c = 0; c < 3; c++) {
            acc[c] = out_layers[j].rgb[c] * a + acc[c] * t;
        }
    }
    out_rgb[0] = acc[0];
    out_rgb[1] = acc[1];
    out_rgb[2] = acc[2];

    return total_trans;
}


//---------------------------------------------------------------------
// 只读解析
//---------------------------------------------------------------------
MomentOITResult MomentOITFrame::Resolve() const
{
    requirePhase(PHASE_TRANSPARENT, "Resolve");

    MomentOITResult result;
    result.width = _width;
    result.height = _height;

    size_t pixels = (size_t)_width * (size_t)_height;
    result.rgb.assign(pixels * 3, 0.0);
    result.transmittance.assign(pixels, 1.0);
    result.layer_count.assign(pixels, 0);
    result.layers.assign(pixels * 2, MomentOITLayer());

    for (size_t p = 0; p < pixels; p++) {
        const double* m = &_moments[p * kMomentsPerPixel];
        double b[4] = { m[0], m[1], m[2], m[3] };
        double g[6] = { m[4], m[5], m[6], m[7], m[8], m[9] };
        const double* bg = &_sealed_rgb[p * 3];

        double d0, w0, d1, w1;
        int n = MomentGaussNodes(b, d0, w0, d1, w1);

        MomentOITLayer local[2];
        double rgb[3];
        double trans = MomentCompose(b, g, bg, n,
                                     d0, w0, d1, w1, local, rgb);

        result.rgb[p * 3 + 0] = rgb[0];
        result.rgb[p * 3 + 1] = rgb[1];
        result.rgb[p * 3 + 2] = rgb[2];
        result.transmittance[p] = trans;
        result.layer_count[p] = n;
        for (int k = 0; k < n; k++) {
            result.layers[p * 2 + k] = local[k];
        }
    }

    return result;
}
