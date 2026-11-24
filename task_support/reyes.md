# 曲面及入口契约

新增mini3d.h暴露原核心类型和函数；reyes.h包含它及stddef.h、stdint.h，声明下列C接口（字段次序可照下列布局）：
```
typedef struct {
    double cp[16][3], color[3], amplitude;
    unsigned ku, kv;
    uint64_t edge_id[4];
    unsigned edge_reverse[4];
} reyes_patch;
typedef struct { double world[3], uv[2]; } reyes_vertex;
typedef struct {
    unsigned max_grid_vertices, max_depth;
    double max_edge_pixels, near_z, far_z;
    double displacement_axis[3], light_dir[3], ambient;
    int occlusion;
    void (*microtriangle)(size_t patch, const reyes_vertex vertices[3], void *user);
    void *user;
} reyes_options;
typedef struct {
    size_t bound_tests, split_nodes, diced_grids, shaded_vertices;
    size_t emitted_triangles, occluded_nodes, peak_grid_vertices, peak_live_vertices;
} reyes_stats;
int reyes_render(device_t *device, const reyes_patch *patches, size_t count,
                 const reyes_options *options, reyes_stats *stats);
```
返回1成功，0参数/分配错误，-1在max_depth内无法满足细度/容量。失败时设备色深、状态、stats保留旧值；调试回调在微三角实际送入暂存光栅时执行；失败时可以留下调试前缀，但不作为设备结果发布。空场景成功且色深保持，统计全零。输入和设备的矩阵、纹理、render_state不改，设备的原颜色/深度作为起点参与遮挡；成功仅发布新色深及统计。只支持RENDER_STATE_COLOR。两台设备或连续帧调用不得共享可变队列/边表/深度缓存。

cp[4*j+i]对应(u=i/3,v=j/3)的双三次Bernstein控制点，u/v在[0,1]。世界曲面为S(u,v)=sum(B_i^3(u)B_j^3(v)cp[4*j+i])+axis*A*sin(2*pi*ku*u)*sin(2*pi*kv*v)。axis和light_dir为世界单位向量，A可正可负，ku/kv为1..8；位移在四条边上精确为0。颜色为有限[0,1]RGB。每个新格点按解析du×dv归一得法线，顶点颜色=color*(ambient+(1-ambient)*max(0,n dot light_dir))，ambient在[0,1]。双面光栅，法线沿参数朝向，不自动转向相机。允许零振幅及平面；输入曲面本身du×dv非零，不要求相邻patch法线连续。采样点的几何/法线不能用最粗四角的线性插值代替。

四边依次为u=0、u=1、v=0、v=1，局部站点参数分别是v、v、u、u。edge_id=0表示不连接，非零id恰好出现在两边；反向标志把局部t映到公共t或1-t。调用方保证对应控制曲线在公共方向上一致，几何坐标相同但id不同不焊接。每条共享边两侧发出完全相同的公共参数站点、坐标及线段，片内子网格相接也无悬挂点；允许局部闭包和过渡三角，不允许整个连通分量统一到最高细度。拓扑/站点元数据可以保留，但不得先物化全场微顶点与微三角数组再光栅。

world矩阵为identity，view为有限刚体，projection由原matrix_set_perspective产生，near_z/far_z匹配该投影且0<near<far。支持眼前、跨眼面/近面、侧面和远面的patch。先对完整位移曲面建立保守界，不能用角点或固定探针代替。被六面完全排除才可丢弃；部分可见的微三角在齐次空间裁出真实部分，再使用原vertex_rhw_init、trapezoid/scanline路径提交，不能另造射线成像或替代光栅核。

max_edge_pixels在[.25,4]，max_depth在1..16，max_grid_vertices在[16,4096]。分裂以原UV中点进行；每个原始微三角经六面裁剪后所有屏幕边长度不大于max_edge_pixels（浮点容差1e-5像素）。计算细度时仍需保守包住曲面弯曲与正弦位移，不能让sin在整步上恰好回到零就漏掉中间的凸起。容量不足继续分裂，不通过截断或丢弃补偿。任何单个活跃网格的求值顶点数不超过max_grid_vertices，同时仍存活的已求值几何顶点总数不超过2倍该值；不把常驻输入控制点、标量参数站点、帧色深/层次深度算作格点。调试调用方自行收集的数据不计入生产工作内存。

按输入patch顺序提交，等深沿用原rhw>=的后画覆盖规则。occlusion=0关掉调度剔除，1允许以当前真正提交的深度建立保守层次，只在投影范围的所有像素已覆盖且严格在整块位移曲面前方时跳过该块求值/发三角。背景深度0、孔洞、等深、非整块画布边缘不能误杀。可以访问初始设备中的不透明深度；不能拿近端的单个样本或块中心代表全范围。两种开关的颜色和浮点深度逐字节一致，剔除块本来就不会获胜。输入patch的空间范围不限制在视口内。

统计在真实边界检查、split、格点评值/着色和发出微三角处增加：bound_tests包括所有投影/遮挡界检查；shaded_vertices包括重新求值的重复边站点；emitted_triangles为提交六面裁剪前的微三角数；occluded_nodes只数深度证明拒绝的块。peak字段为真实峰值。回调顺序随输入patch与内部UV有序遍历，提供发出的裁前世界顶点与UV，用于独立检查；不可只为回调生成另一张网格。默认NULL。

正常平面、弯曲/位移片、共享曲边、近裁片必须真实完成；max_depth极小的明确超限场景才用-1。用前景遮住大后片时，剔除开启要使shaded_vertices和emitted_triangles都严格减少；留孔后其像素必须恢复。局部高频片与平坦邻片相接时，远离共边的平坦区域不能继承高频内部细度。允许保守少剔除，不允许改变采样/成图。
