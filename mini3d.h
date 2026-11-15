//=====================================================================
//
// mini3d.h - Mini Software Render 公共接口（C99，无窗口/GPU/第三方依赖）
//
// 本头文件暴露原 mini3d.c 的矩阵、梯形、扫描线与软件设备接口，供离线
// 焦散模块（caustics.h）与测试程序复用。所有类型均为值语义，调用方
// 负责 device_t 自身的存储；设备只持有外部帧缓存的引用，不取得所有权。
//
//=====================================================================
#ifndef MINI3D_H
#define MINI3D_H

typedef unsigned int IUINT32;

//=====================================================================
// 数学库：标准 D3D 行向量矩阵，y = x * m
//=====================================================================
typedef struct { float m[4][4]; } matrix_t;
typedef struct { float x, y, z, w; } vector_t;
typedef vector_t point_t;

int CMID(int x, int min, int max);

// 计算插值：t 为 [0, 1] 之间的数值
float interp(float x1, float x2, float t);

// | v |
float vector_length(const vector_t *v);

// z = x + y
void vector_add(vector_t *z, const vector_t *x, const vector_t *y);

// z = x - y
void vector_sub(vector_t *z, const vector_t *x, const vector_t *y);

// 矢量点乘
float vector_dotproduct(const vector_t *x, const vector_t *y);

// 矢量叉乘
void vector_crossproduct(vector_t *z, const vector_t *x, const vector_t *y);

// 矢量插值，t取值 [0, 1]
void vector_interp(vector_t *z, const vector_t *x1, const vector_t *x2, float t);

// 矢量归一化
void vector_normalize(vector_t *v);

// c = a + b
void matrix_add(matrix_t *c, const matrix_t *a, const matrix_t *b);

// c = a - b
void matrix_sub(matrix_t *c, const matrix_t *a, const matrix_t *b);

// c = a * b
void matrix_mul(matrix_t *c, const matrix_t *a, const matrix_t *b);

// c = a * f
void matrix_scale(matrix_t *c, const matrix_t *a, float f);

// y = x * m
void matrix_apply(vector_t *y, const vector_t *x, const matrix_t *m);

void matrix_set_identity(matrix_t *m);
void matrix_set_zero(matrix_t *m);

// 平移变换
void matrix_set_translate(matrix_t *m, float x, float y, float z);

// 缩放变换
void matrix_set_scale(matrix_t *m, float x, float y, float z);

// 旋转矩阵
void matrix_set_rotate(matrix_t *m, float x, float y, float z, float theta);

// 设置摄像机
void matrix_set_lookat(matrix_t *m, const vector_t *eye, const vector_t *at,
	const vector_t *up);

// D3DXMatrixPerspectiveFovLH
void matrix_set_perspective(matrix_t *m, float fovy, float aspect, float zn,
	float zf);


//=====================================================================
// 坐标变换
//=====================================================================
typedef struct {
	matrix_t world;         // 世界坐标变换
	matrix_t view;          // 摄影机坐标变换
	matrix_t projection;    // 投影变换
	matrix_t transform;     // transform = world * view * projection
	float w, h;             // 屏幕大小
}	transform_t;

// 矩阵更新，计算 transform = world * view * projection
void transform_update(transform_t *ts);

// 初始化，设置屏幕长宽
void transform_init(transform_t *ts, int width, int height);

// 将矢量 x 进行 project
void transform_apply(const transform_t *ts, vector_t *y, const vector_t *x);

// 检查齐次坐标同 cvv 的边界用于视锥裁剪
int transform_check_cvv(const vector_t *v);

// 归一化，得到屏幕坐标
void transform_homogenize(const transform_t *ts, vector_t *y, const vector_t *x);


//=====================================================================
// 几何计算：顶点、扫描线、边缘、矩形、步长计算
//=====================================================================
typedef struct { float r, g, b; } color_t;
typedef struct { float u, v; } texcoord_t;

// world 字段保存透视校正用的世界坐标（rhw 空间内为 world/w_vertex），
// 供焦散照明在片元处还原世界位置；不传时由绘制入口自动填充。
typedef struct {
	point_t pos;
	texcoord_t tc;
	color_t color;
	float rhw;
	point_t world;
} vertex_t;

typedef struct { vertex_t v, v1, v2; } edge_t;
typedef struct { float top, bottom; edge_t left, right; } trapezoid_t;
typedef struct { vertex_t v, step; int x, y, w; } scanline_t;

void vertex_rhw_init(vertex_t *v);
void vertex_interp(vertex_t *y, const vertex_t *x1, const vertex_t *x2, float t);
void vertex_division(vertex_t *y, const vertex_t *x1, const vertex_t *x2, float w);
void vertex_add(vertex_t *y, const vertex_t *x);

// 根据三角形生成 0-2 个梯形，并且返回合法梯形的数量
int trapezoid_init_triangle(trapezoid_t *trap, const vertex_t *p1,
	const vertex_t *p2, const vertex_t *p3);

// 按照 Y 坐标计算出左右两条边纵坐标等于 Y 的顶点
void trapezoid_edge_interp(trapezoid_t *trap, float y);

// 根据左右两边的端点，初始化计算出扫描线的起点和步长
void trapezoid_init_scan_line(const trapezoid_t *trap, scanline_t *scanline,
	int y);


//=====================================================================
// 渲染设备
//=====================================================================
struct caustics_map;     // 前向声明，具体定义见 caustics.h

// 焦散着色钩子：由 caustics 模块绑定，mini3d 自身不提供实现，因此
// mini3d.c 可独立链接。world 为片元经透视插值还原的世界坐标；返回 1
// 表示该点在接收平面容差内并已把结果写入 out，返回 0 表示保持原色。
typedef int (*device_caustics_shade_fn)(const struct caustics_map *map,
	const point_t *world, float tolerance, float ambient, float gain,
	const color_t *base, color_t *out);

typedef struct {
	transform_t transform;      // 坐标变换器
	int width;                  // 窗口宽度
	int height;                 // 窗口高度
	IUINT32 **framebuffer;      // 像素缓存：framebuffer[y] 代表第 y行
	float **zbuffer;            // 深度缓存：zbuffer[y] 为第y行指针
	IUINT32 **texture;          // 纹理：同样是每行索引
	int tex_width;              // 纹理宽度
	int tex_height;             // 纹理高度
	float max_u;                // 纹理最大宽度：tex_width - 1
	float max_v;                // 纹理最大高度：tex_height - 1
	int render_state;           // 渲染状态
	IUINT32 background;         // 背景颜色
	IUINT32 foreground;         // 线框颜色
	// 焦散接收图绑定：NULL 表示未绑定，绘制路径与原始实现完全一致。
	// 设备只借用指针，不取得所有权，绘制过程不会改写该图。
	struct caustics_map *caustics;
	float caustics_tolerance;   // 接收平面世界空间容差，非负
	float caustics_ambient;     // 环境光系数 ambient，非负
	float caustics_gain;        // 焦散增益 gain，非负
	device_caustics_shade_fn caustics_shade;  // 由 caustics 模块安装的钩子
}	device_t;

#define RENDER_STATE_WIREFRAME      1		// 渲染线框
#define RENDER_STATE_TEXTURE        2		// 渲染纹理
#define RENDER_STATE_COLOR          4		// 渲染颜色

// 设备初始化，fb为外部帧缓存，非 NULL 将引用外部帧缓存（每行 4字节对齐）
void device_init(device_t *device, int width, int height, void *fb);

// 删除设备：仅释放内部分配的指针表与深度缓存，外部 fb 由调用方释放
void device_destroy(device_t *device);

// 设置当前纹理，bits 由调用方持有，设备只保存行指针
void device_set_texture(device_t *device, void *bits, long pitch, int w, int h);

// 清空 framebuffer 和 zbuffer
void device_clear(device_t *device, int mode);

// 画点
void device_pixel(device_t *device, int x, int y, IUINT32 color);

// 绘制线段
void device_draw_line(device_t *device, int x1, int y1, int x2, int y2,
	IUINT32 c);

// 根据坐标读取纹理
IUINT32 device_texture_read(const device_t *device, float u, float v);

// 绘制扫描线
void device_draw_scanline(device_t *device, scanline_t *scanline);

// 主渲染函数
void device_render_trap(device_t *device, trapezoid_t *trap);

// 根据 render_state 绘制原始三角形
void device_draw_primitive(device_t *device, const vertex_t *v1,
	const vertex_t *v2, const vertex_t *v3);

#endif // MINI3D_H
