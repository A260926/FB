//=====================================================================
//
// caustics.h - 离线确定性光束通量焦散通道（C99）
//
// 单界面模型：平行光经玻璃表面折射一次后落到接收平面上，不追踪第二
// 次出玻璃。整个通道只使用 CPU 与浮点运算，不依赖窗口、GPU 或任何第
// 三方库。所有权规则：
//   - 所有几何输入（顶点、法线、透过率、矩阵、纹理）均按只读引用传入，
//     捕获期间不修改；调用方负责其生命周期，只需覆盖本次调用即可。
//   - caustics_map 由 caustics_map_create 动态分配，caustics_map_release
//     释放；绑定到 device_t 只是借用指针，设备不取得所有权。
//
//=====================================================================
#ifndef CAUSTICS_H
#define CAUSTICS_H

#include "mini3d.h"

#ifdef __cplusplus
extern "C" {
#endif

// 捕获输入：世界空间三角形网格 + 逐顶点法线 + 逐面 RGB 透过率
typedef struct {
	int triangle_count;       // 三角形数量，必须 >= 0（0 表示空场景）
	const point_t *positions; // 长度 3*triangle_count 的世界坐标，w 恒为 1
	const vector_t *normals;  // 长度 3*triangle_count 的逐顶点单位法线，
	                          // 朝向入射侧（指向光传播来的一侧）
	const color_t *transmit;  // 长度 triangle_count 的逐面 RGB 透过率，
	                          // 各分量非负且不超过 1
	const matrix_t *light;    // 世界到光图的正交矩阵，输入 w 恒为 1，
	                          // 矩阵本身不做要求；其光空间 +z 轴在世界中
	                          // 的方向即光传播方向，-1..1 的 NDC 覆盖整个
	                          // 孔径（aperture_width/height 为垂直光线的
	                          // 世界截面尺寸）
	float aperture_width;     // 光孔径世界宽，必须 > 0
	float aperture_height;    // 光孔径世界高，必须 > 0
	const color_t *incident;  // 光的 RGB 入射辐照度（3 分量），非负
	int light_width;          // 光图宽，像素数，必须 >= 2
	int light_height;         // 光图高，像素数，必须 >= 2
	float ior_in;             // 入射侧折射率，必须 > 0
	float ior_out;            // 出射侧折射率，必须 > 0
} caustics_capture_input;

// 接收平面定位：平面原点 origin，正交单位轴 u/v，以及 u/v 方向的世界范围
typedef struct {
	point_t origin;           // 接收平面原点（u=0,v=0 处的世界点）
	vector_t axis_u;          // 单位 u 轴
	vector_t axis_v;          // 单位 v 轴，与 axis_u 正交
	float range_u_min;        // u 方向世界范围（含），min < max
	float range_u_max;
	float range_v_min;        // v 方向世界范围，min < max
	float range_v_max;
	int map_width;            // 接收图宽，像素数，必须 >= 1
	int map_height;           // 接收图高，像素数，必须 >= 1
} caustics_receiver;

// 单个光图像素中心样本（只读查询用）
typedef struct {
	int valid;                // 1 有效；0 无效（无覆盖/全反射/射线不相交）
	point_t position;         // 最近折射面的世界位置（有效时）
	vector_t normal;          // 该处归一化世界法线（有效时）
	int face_id;              // 原面 ID（输入三角形下标，从 0 起）
	float depth;              // 光空间 z/w，无效样本为 +INFINITY
	point_t hit;              // 折射射线在接收平面上的世界落点（有效时）
} caustics_sample;

// 不透明焦散图对象
typedef struct caustics_map caustics_map;

// 创建接收图。返回 NULL 表示参数非法或内存不足；接收平面配置在整个
// 对象生命周期内固定。返回对象的初始图为全 0 空图。
caustics_map *caustics_map_create(const caustics_receiver *receiver);

// 释放对象及其全部内部缓冲。map 为 NULL 时为空操作。
void caustics_map_release(caustics_map *map);

// 执行一次完整捕获：光图光栅化 -> Snell 折射 -> 光束足迹沉积。
// 每次捕获从空图开始；输入不会被修改。任何内部失败都不会发布半幅
// 结果——失败时对象保持上一次成功捕获后的状态（新对象则保持全 0）。
// 返回 0 成功，负值表示参数非法或内存不足。
int caustics_capture(caustics_map *map, const caustics_capture_input *input);

// 读取光图像素 (x,y) 的样本（像素中心）。越界返回全无效样本。
caustics_sample caustics_get_sample(const caustics_map *map, int x, int y);

// 接收图尺寸
int caustics_map_width(const caustics_map *map);
int caustics_map_height(const caustics_map *map);

// 接收纹素 (x,y) 的 RGB 平均辐照度（单位接收纹素面积上的辐照度）。
// 越界返回 0。
void caustics_get_irradiance(const caustics_map *map, int x, int y,
	float *r, float *g, float *b);

// 在接收平面世界坐标 (u,v) 处双线性采样；图外返回 0。
void caustics_sample_irradiance(const caustics_map *map, float u, float v,
	float *r, float *g, float *b);

// 把接收图绑定到软件设备：之后该设备的颜色/纹理片元若经透视插值得到
// 的世界位置落在接收平面 tolerance（非负）容差内，就叠加焦散照明。
// 绑定只影响接收平面，不改变原 rhw 遮挡与纹理覆盖顶点色的优先级；
// 绘制不会写回该图，多设备与重复绘制互不污染。
// ambient/gain 均为非负系数，最终颜色 = 底色 * (ambient + gain*irradiance)
// 后一次量化。返回 0 成功，负值参数非法。
int caustics_bind_device(caustics_map *map, device_t *device, float tolerance,
	float ambient, float gain);

// 解绑：设备恢复未绑定时的原始输出路径。
void caustics_unbind_device(device_t *device);

#ifdef __cplusplus
}
#endif

#endif // CAUSTICS_H
