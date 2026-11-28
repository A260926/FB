在 src/irregular_shadow.h 定义下列 C99 接口，实现在同名 .c。头文件包含 mytinygl.h，函数使用显式 ctx；返回1成功、0失败，不改变当前上下文绑定、矩阵、材质、灯或纹理状态。成功时只更新本次相机深度胜出的颜色和深度，其他像素保留；统计只含本次最终胜出的接收样本。支持条件见任务说明；调用不在glBegin/glEnd或显示列表编译中。输入三角形共3*triangle_count个顶点，每个 position 的物体空间齐次w=1，normal/UV沿用原提交变换；glColor使用上下文当前颜色。所有输入有限、法线有效、索引无需外部资源。triangle_count=0时vertices可为NULL，返回成功并把stats清零；stats必非NULL。

```c
typedef struct { GLfloat position[3], normal[3], uv[2]; } mtgl_shadow_vertex;
typedef struct { unsigned long long camera_samples, shadowed_samples, hierarchy_nodes, sample_triangle_tests; } mtgl_shadow_stats;
int mtgl_render_irregular_shadow(GLState *ctx, const mtgl_shadow_vertex *vertices, size_t triangle_count, const GLfloat light_clip_from_eye[16], GLenum selected_light, GLfloat bias, mtgl_shadow_stats *stats);
```

矩阵按原OpenGL列主序，selected_light是GL_LIGHT0..GL_LIGHT7。光矩阵为非奇异正交投影仿射变换，最后一行为0,0,0,1，调用方保证沿+光深度是背离所选灯方向；x/y/z的clip体为[-1,1]。深度比较前统一取 (z_clip+1)/2；bias也用这个[0,1]尺度。每个光裁剪fan是一个实际测试三角，每次叶内实际点覆盖/深度测试都计一次sample_triangle_tests；可提前跳过已确定为阴影的样本，不虚增/虚减计数。叶最多8个样本，只有达到24层或坐标完全重合才允许溢出；划分规则/中心线归属需要确定且不能丢样本。节点包围盒和有向边范围必须保守，不能按节点中心决定是否相交。仅本次输入三角形作遮挡物；先前帧缓冲只参加相机深度竞争，不凭空成为本批遮挡几何。输入顺序按原GL_LESS契约竞争；原三角身份保留到相机记录、光裁剪和统计。测试可用独立射线参照，生产阴影算法不可逐接收点追线。
