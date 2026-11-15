//=====================================================================
//
// soft_raster.h - RenderHelp 的可微软光栅通道
//
// 用于小范围几何拟合：软帧期间逐三角形提交，结束时得到 double 精度
// RGB 图像与拥有型前向记录；反向入口接收逐像素 RGB 伴随，返回每个
// 原始提交面三个角的齐次坐标 (xyzw) 与线性 RGB 梯度，即
// d[sum_pixel(g . image)] / d[VS 输出]。
//
// 特性：
//
// - 复用 RenderHelp 的 VS 回调、齐次裁剪体投影与透视属性插值约定
// - 覆盖：Sutherland-Hodgman 齐次裁剪 + 外边平滑垂距的 smoothstep 积
// - 遮挡：log(p) - z/tau 与背景 log 权重 -1/tau 的稳定 softmax 混合
// - 反向为解析微分，不扰动参数、不做有限差分、不重绘；裁剪交点、
//   屏幕重心、归一分母均参与求导，仅分支选择沿用前向，clamp 两端
//   导数为 0
// - 记录深拷贝全部前向值，VS 捕获对象销毁 / 另画新帧 / 多次反传 /
//   记录间相互独立，均不影响旧结果
//
// 软帧不执行像素着色器，仅支持 varying_vec3f 指定槽位的线性 RGB
// 模型；结束不改动 RenderHelp 原有的硬帧颜色/深度缓存。
//
// 错误处理约定：
//
// - 入参非法（空渲染器、未设置 VS、宽高非正、sigma/tau 非正、
//   varying 槽缺失、VS 返回非有限值或 w<=0）时抛出
//   std::invalid_argument，软帧状态自动回滚，不会残留半开帧
// - SoftFrameRecord 为拥有型对象，可拷贝/移动；对同一记录或其副本
//   多次调用 Backward 互不干扰，每次梯度从零累积
// - 反向伴随尺寸与记录不一致时抛出 std::invalid_argument
//
//=====================================================================
#ifndef _SOFT_RASTER_H_
#define _SOFT_RASTER_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>
#include <memory>
#include <stdexcept>

#include "RenderHelp.h"


//---------------------------------------------------------------------
// 单个提交面的反向梯度：三个角，每角齐次 xyzw + 线性 RGB 三分量
//---------------------------------------------------------------------
struct SoftFaceGradient {
	Vec4d position[3];    // dL/d(pos.x, pos.y, pos.z, pos.w)
	Vec3d color[3];       // dL/d(r, g, b)
};


//---------------------------------------------------------------------
// 前向记录（拥有型，可安全拷贝/移动，可反复反传）
//---------------------------------------------------------------------
class SoftFrameRecord {
public:
	SoftFrameRecord();
	~SoftFrameRecord();
	SoftFrameRecord(const SoftFrameRecord& other);
	SoftFrameRecord(SoftFrameRecord&& other) noexcept;
	SoftFrameRecord& operator = (const SoftFrameRecord& other);
	SoftFrameRecord& operator = (SoftFrameRecord&& other) noexcept;

	int Width() const;
	int Height() const;
	size_t FaceCount() const;       // 原始提交面数量（含零贡献面）

private:
	friend class SoftRasterContext;
	struct Impl;
	std::unique_ptr<Impl> _impl;
};


//---------------------------------------------------------------------
// 软帧上下文：Begin / SubmitTriangle / End 三段式
//
// 典型用法：
//
//   SoftRasterContext soft(rh);
//   soft.Begin(64, 48, bg, 1.0 /*sigma*/, 1.0 /*tau*/, 0 /*slot*/);
//   soft.SubmitTriangle();         // 可重复，内部对 VS 每角调一次
//   auto record = soft.End(image); // image 为输出的 double RGB 图
//   auto grads  = SoftRasterContext::Backward(*record, adj);
//---------------------------------------------------------------------
class SoftRasterContext {
public:
	explicit SoftRasterContext(RenderHelp& rh);
	~SoftRasterContext();

	// 开始一帧：width/height 输出图尺寸，background 线性 RGB，
	// sigma 为屏幕像素单位的软边宽度 (>0)，tau 为深度竞争温度 (>0)，
	// color_slot 指定从 VS 输出的哪个 varying_vec3f 槽取线性 RGB。
	// 若已有未结束的软帧会抛 std::logic_error。
	void Begin(int width, int height, const Vec3f& background,
		double sigma, double tau, int color_slot);

	// 提交一个三角形：调用当前 RenderHelp 的 VS，每个角恰好一次，
	// 取齐次坐标与指定槽 RGB。仅在 Begin/End 之间可用，否则抛
	// std::logic_error。
	void SubmitTriangle();

	// 结束软帧：执行软光栅，rgb_image 输出按行存储的 double RGB
	// （大小 3*width*height，通道顺序 r,g,b），返回拥有型记录。
	// 结束后上下文可再次 Begin 开启新帧；本调用不触碰硬帧缓存。
	SoftFrameRecord End(std::vector<double>& rgb_image);

	// 反向：adj_rgb 与记录同尺寸的逐像素 RGB 伴随，返回每个提交面
	// 三个角的梯度（对应标量损失 sum(adj . image)）。每次调用梯度
	// 都从 0 开始；只对 VS 输出的齐次坐标与 RGB 求导。
	static std::vector<SoftFaceGradient> Backward(
		const SoftFrameRecord& record,
		const std::vector<double>& adj_rgb);

private:
	SoftRasterContext(const SoftRasterContext&) = delete;
	SoftRasterContext& operator = (const SoftRasterContext&) = delete;

	RenderHelp& _rh;
	bool _active;
	int _width;
	int _height;
	int _color_slot;
	double _sigma;
	double _tau;
	Vec3d _background;

	// 提交期缓存的原始 VS 输出（深拷贝），End 时转入记录
	struct RawFace;
	std::vector<RawFace> _faces;
};


#endif
