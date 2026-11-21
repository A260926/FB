//=====================================================================
//
// moment_oit.h - RenderHelp 的矩透明帧（Moment OIT）离屏预览扩展
//
// 用法（调用次序）：
//
//   RenderHelp rh(w, h);
//   MomentOit mo(rh);                 // 宿主必须先声明、后析构
//   rh.SetVertexShader(vs);           // 沿用现有的 VS/PS/ShaderContext
//   rh.SetPixelShader(ps);
//   mo.BeginFrame(Vec3d(r, g, b));    // 开始矩帧，传入线性 RGB 背景
//   rh.DrawPrimitive();               // 画不透明面（若干次）
//   mo.SealOpaque();                  // 封存不透明颜色和窗口深度
//   rh.DrawPrimitive();               // 收集透明面（若干次）
//   MomentOitPixel px = mo.ResolvePixel(x, y);   // 只读解析（可多次）
//   MomentOitMoments m = mo.GetMoments(x, y);    // 读取每像素原始矩
//   mo.CancelFrame();                 // 取消矩帧，之后可以重新开始
//
// 透明阶段每像素只保留 10 个 double 的矩（另有 10 个 Kahan 补偿累加量），
// 不存片元链表，解析时从矩重建至多两层再做合成，属于矩模型的近似：
// 不超过两个不同深度时等价于精确分层合成，更多层时是两节点高斯求积近似。
//
//=====================================================================
#ifndef _MOMENT_OIT_H_
#define _MOMENT_OIT_H_

#include "RenderHelp.h"

#include <vector>


//---------------------------------------------------------------------
// 数据结构
//---------------------------------------------------------------------

// 透明阶段每像素保存的原始矩，固定 10 个 double
struct MomentOitMoments {
	double b[4];       // 光学矩 b[k] = sum(tau * z^k)，k = 0..3
	double g[3][2];    // 颜色矩 g[c][k] = sum(tau * RGB[c] * z^k)，k = 0..1

	inline MomentOitMoments() {
		for (int k = 0; k < 4; k++) b[k] = 0.0;
		for (int c = 0; c < 3; c++) g[c][0] = g[c][1] = 0.0;
	}
};

// 从矩重建出的一层（按深度由近到远排列）
struct MomentOitLayer {
	double depth;            // 节点深度 d，范围 [0, 1]
	double optical_mass;     // 光学质量 w > 0（该层 tau 的总量）
	double alpha;            // 层不透明度 alpha = -expm1(-w)
	double color[3];         // 逐通道钳到 [0, 1] 的层色（用于合成）
	double unclamped[3];     // 未钳制的层色，便于核对颜色矩

	inline MomentOitLayer() {
		depth = optical_mass = alpha = 0.0;
		for (int c = 0; c < 3; c++) color[c] = unclamped[c] = 0.0;
	}
};

// 单个像素的只读解析结果，独立持有（之后的绘制和解析都不影响它）
struct MomentOitPixel {
	double color[3];                 // 合成后的线性 RGB（含封存颜色项）
	double transmittance;            // 透射率 exp(-b[0])
	std::vector<MomentOitLayer> layers;    // 重建层，0..2 层，由近到远

	inline MomentOitPixel() {
		for (int c = 0; c < 3; c++) color[c] = 0.0;
		transmittance = 1.0;
	}
};


//---------------------------------------------------------------------
// MomentOit：矩透明帧管理器，作为钩子安装到宿主 RenderHelp 上
//---------------------------------------------------------------------
class MomentOit : public RenderHelpMomentHook {
public:

	// 矩帧阶段
	enum Phase {
		PHASE_IDLE = 0,      // 空闲：没有活动矩帧
		PHASE_OPAQUE = 1,    // 不透明收集阶段（BeginFrame 之后）
		PHASE_SEALED = 2     // 已封存：透明收集阶段（SealOpaque 之后），可解析
	};

	// 宿主必须比 MomentOit 活得更久（先声明 RenderHelp，再声明 MomentOit）
	explicit MomentOit(RenderHelp &host);
	virtual ~MomentOit();

	// 开始一个矩帧：传入线性 RGB 背景，进入不透明收集阶段。
	// 要求当前没有活动矩帧、宿主已 Init、宿主没有被其他矩帧占用，
	// 否则抛 std::logic_error（抛出前不修改任何状态）。
	void BeginFrame(const Vec3d &background);

	// 封存不透明阶段的颜色和窗口深度，进入透明收集阶段。
	// 要求当前处于不透明阶段，否则抛 std::logic_error。
	void SealOpaque();

	// 取消当前矩帧（幂等，不会抛异常）；取消后可以重新开始。
	// 宿主的 Clear/Init/Reset 也会触发取消。
	void CancelFrame();

	// 当前阶段
	inline Phase GetPhase() const { return _phase; }

	// 读取每像素原始矩（只读）；要求已封存，否则抛 std::logic_error
	MomentOitMoments GetMoments(int x, int y) const;

	// 只读解析单个像素：返回独立持有的 RGB、透射率和重建层。
	// 不执行 VS/PS，不写普通帧的颜色和深度，可多次调用结果一致。
	// 要求已封存，否则抛 std::logic_error。
	MomentOitPixel ResolvePixel(int x, int y) const;

	// 只读解析整帧，按行主序返回 w*h 个像素结果
	std::vector<MomentOitPixel> ResolveFrame() const;

	// RenderHelpMomentHook 接口实现
	virtual bool OnDrawPrimitive(RenderHelp &rh);
	virtual void OnFrameReset();

protected:

	// 每像素的矩存储：10 个矩 + 10 个 Kahan 补偿累加量（固定 20 个 double）
	struct PixelMoments {
		double b[4];         // 光学矩
		double g[3][2];      // 颜色矩
		double cb[4];        // b 的 Kahan 补偿
		double cg[3][2];     // g 的 Kahan 补偿
		inline PixelMoments() {
			for (int k = 0; k < 4; k++) b[k] = cb[k] = 0.0;
			for (int c = 0; c < 3; c++) g[c][0] = g[c][1] = cg[c][0] = cg[c][1] = 0.0;
		}
	};

	// 矩帧自己的光栅化路径（不透明/透明两个阶段共用裁剪、覆盖和属性遍历）
	bool Rasterize(RenderHelp &rh, bool transparent);

	// 要求处于已封存阶段，否则抛 std::logic_error（const 的只读检查）
	void RequireSealed() const;
	// 检查像素坐标
	void RequireCoord(int x, int y) const;

protected:

	RenderHelp *_host;             // 宿主渲染器
	Phase _phase;                  // 当前阶段
	int _w, _h;                    // 帧尺寸（BeginFrame 时从宿主取得）

	std::vector<double> _color;    // 封存颜色：每像素 3 个 double 的线性 RGB
	std::vector<double> _depth;    // 窗口深度：每像素 1 个 double，背景为正无穷
	std::vector<PixelMoments> _mom;    // 每像素矩（含 Kahan 补偿）
};


#endif
