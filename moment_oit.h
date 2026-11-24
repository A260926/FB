//=====================================================================
//
// moment_oit.h - RenderHelp 的可选矩透明（Moment OIT）离屏预览帧
//
// 用途：在不改动 RenderHelp 普通帧行为的前提下，用固定的每像素存储量
// （恰好十个 double）支持相互穿插的半透明色片的顺序无关透明合成。
//
// 调用次序（详见 README.md）：
//
//   MomentOITFrame frame(rh);                 // 绑定一台 RenderHelp
//   frame.BeginFrame(r, g, b);                // 传入线性 RGB 背景色
//   rh.SetVertexShader(...); rh.SetPixelShader(...);
//   rh.DrawPrimitive(); ...                   // 不透明阶段（PS 的 alpha<=0 丢弃）
//   frame.SealOpaque();                       // 封存颜色与窗口深度
//   rh.DrawPrimitive(); ...                   // 透明阶段（收集矩，不占深度）
//   MomentOITResult res = frame.Resolve();    // 只读解析，不执行 VS/PS
//   frame.Cancel();                           // 或交给析构
//
// 仅使用 C++17 标准库，不依赖窗口、GPU 或任何第三方库。
//
//=====================================================================
#ifndef _MOMENT_OIT_H_
#define _MOMENT_OIT_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "RenderHelp.h"


//---------------------------------------------------------------------
// 矩透明重建出的一个层（节点），全部为独立持有的 double
//---------------------------------------------------------------------
struct MomentOITLayer {
    double depth;        // 重建深度 d（窗口空间，越小越近）
    double weight;       // 正的光学质量 w（光学厚度）
    double transmittance;// 该层自身透射率 exp(-w)
    double alpha;        // 该层自身不透明度 -expm1(-w)
    double rgb[3];       // 逐通道钳到 [0,1] 的层色
    double rgb_raw[3];   // 未钳制的层色，方便核对矩
};


//---------------------------------------------------------------------
// 一次解析的完整结果，按像素独立持有，解析后与原帧不再共享任何状态
//---------------------------------------------------------------------
struct MomentOITResult {
    int width = 0;
    int height = 0;
    std::vector<double> rgb;             // 最终颜色 RGB，长度 width*height*3
    std::vector<double> transmittance;   // 每像素透明总透射率 exp(-b0)，长度 width*height
    std::vector<MomentOITLayer> layers;  // 每像素 0~2 个重建层，长度 width*height*2
    std::vector<int> layer_count;        // 每像素实际层数（0/1/2），长度 width*height

    inline int GetWidth() const { return width; }
    inline int GetHeight() const { return height; }

    // 取像素基址
    inline size_t PixelIndex(int x, int y) const {
        return (size_t)y * (size_t)width + (size_t)x;
    }

    // 最终颜色（线性 RGB，已含背景）
    inline const double* GetRGB(int x, int y) const {
        return &rgb[PixelIndex(x, y) * 3];
    }

    inline double GetTransmittance(int x, int y) const {
        return transmittance[PixelIndex(x, y)];
    }

    inline int GetLayerCount(int x, int y) const {
        return layer_count[PixelIndex(x, y)];
    }

    inline const MomentOITLayer& GetLayer(int x, int y, int k) const {
        return layers[PixelIndex(x, y) * 2 + (size_t)k];
    }
};


//---------------------------------------------------------------------
// MomentOITFrame：矩透明离屏帧
//
// 生命周期状态：
//   Idle      -> BeginFrame -> Opaque -> SealOpaque -> Transparent
//   Transparent -> Resolve（可多次，只读）/ Cancel -> Idle
// 任何阶段误用都会在修改任何状态之前抛出 std::logic_error。
//---------------------------------------------------------------------
class MomentOITFrame {
public:

    enum Phase {
        PHASE_IDLE = 0,        // 没有活动帧
        PHASE_OPAQUE = 1,      // 不透明绘制阶段
        PHASE_TRANSPARENT = 2  // 透明收集阶段（已封存）
    };

    // 透明阶段每像素固定保留的 double 数量（四个光学矩 + 两个 RGB 矩）
    static constexpr int kMomentsPerPixel = 10;

public:

    explicit MomentOITFrame(RenderHelp& rh);
    ~MomentOITFrame();

    // 开始一帧：传入线性 RGB 背景（各通道有限并钳到 [0,1]），清空全部缓存
    void BeginFrame(double bg_r, double bg_g, double bg_b);

    // 封存不透明阶段：快照当前颜色与窗口深度（z/w，背景为 +inf），
    // 随后进入透明收集阶段
    void SealOpaque();

    // 只读解析：不执行任何 VS/PS，不写 RenderHelp 的普通帧颜色与深度；
    // 可多次调用，每次返回独立持有的结果
    MomentOITResult Resolve() const;

    // 取消活动帧并回到 Idle，取消后可重新 BeginFrame
    void Cancel();

    // 直接读取某像素的原始矩（长度 10）与封存的窗口深度，供核对；
    // 仅在透明阶段可调用
    void GetPixelMoments(int x, int y, double out_moments[10]) const;
    double GetSealedDepth(int x, int y) const;

    Phase GetPhase() const { return _phase; }
    int GetWidth() const { return _width; }
    int GetHeight() const { return _height; }

public:
    // 供 RenderHelp 内部光栅器回调，普通帧不会触发
    void onOpaqueFragment(int x, int y, double depth,
                          double r, double g, double b);
    void onTransparentFragment(int x, int y, double depth,
                               double r, double g, double b, double alpha);

private:
    void requirePhase(Phase expected, const char* what) const;
    void allocateStorage();

private:
    RenderHelp& _rh;
    Phase _phase;

    int _width;
    int _height;

    // 封存的不透明颜色（线性 RGB，来自 BeginFrame 的背景或不透明 PS）
    std::vector<double> _sealed_rgb;
    // 封存的窗口深度 z/w，背景为 +inf
    std::vector<double> _sealed_depth;
    // 透明阶段每像素十个 double：b0..b3, g0r,g0g,g0b, g1r,g1g,g1b
    std::vector<double> _moments;

    // 禁止拷贝，避免回调悬挂
    MomentOITFrame(const MomentOITFrame&) = delete;
    MomentOITFrame& operator=(const MomentOITFrame&) = delete;
};


//---------------------------------------------------------------------
// 纯函数：二节点高斯求积恢复 + 层色重建，公开出来方便单元测试
//---------------------------------------------------------------------

// 由四个光学矩 b[0..3] 恢复至多两个有序深度 d0<=d1 与正的光学质量
// w0,w1，满足 sum(wj * dj^k) = b[k]。单深度退化为一个节点；空像素
// 返回层数 0。保证不出现 NaN 或负质量。
int MomentGaussNodes(const double b[4],
                     double& d0, double& w0,
                     double& d1, double& w1);

// 由节点与六个颜色矩恢复层色（含未钳制值），按近到远合成最终颜色。
// bg 为封存颜色，返回总透射率 exp(-b0)。
double MomentCompose(const double b[4],
                     const double g[6],
                     const double bg[3],
                     int node_count,
                     double d0, double w0, double d1, double w1,
                     MomentOITLayer out_layers[2],
                     double out_rgb[3]);


#endif // _MOMENT_OIT_H_
