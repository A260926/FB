C++ 在 namespace xatlas 中新增：
```cpp
bool SetMeshChartConstraints(Atlas*, uint32_t meshIndex, const uint8_t* flags, uint32_t count);
bool ComputeChartsConstrained(Atlas*, ChartOptions options = ChartOptions());
```
C 在 xatlas_c.h 中新增对应导出：
```c
XATLAS_API bool xatlasSetMeshChartConstraints(xatlasAtlas*, uint32_t meshIndex, const uint8_t* flags, uint32_t count);
XATLAS_API bool xatlasComputeChartsConstrained(xatlasAtlas*, const xatlasChartOptions*);
```
C options 为 NULL 时用默认值。两条路径必须真实共用实现；旧公开结构字段布局与初始化函数不变。

新路径仅支持全部由 AddMesh 加入的非退化、定向二流形三角网；允许边界、多个分量、位置重合但索引不同的分量，不支持 faceVertexCount、忽略面、AddUvMesh、useInputMeshUvs=true 和自定义 paramFunc。输入可有正常法线/UV seam 以及多材质。非有限、退化或不支持输入在新路径中明确拒绝。旧路径范围和行为不改。

meshIndex 为 AddMesh 次序；flags 按原输入角顺序，角 c 表示该面从 c 到 (c+1)%3 的有向边，按 indexOffset 生效后的原索引识别邻接，不能因 epsilon 共点把断开分量连起来。count 必须等于原三角面数乘3；count=0且flags=nullptr 清除该mesh约束，其他空指针/坏计数/越界mesh/未知标记返回 false。输入复制并等待必要的 AddMeshJoin；返回后输入可释放。一个共享边的两侧标记按 0+非零 得非零、相同非零保持、1+2冲突处理，2落在真实边界也冲突，1落在边界可视为已有切缝。setter 对语法、边约束冲突失败不改变旧约束或旧输出；语义组件冲突最迟由 compute 检出。

Join沿原相邻面作传递闭包。组件每次必须整体入图表，任何阶段不得分拆：PlanarCharts、ClusteredCharts 的生长/重播种/填洞/合并，以及 Chart 参数化和 Piecewise 重切都适用。硬 Cut 的两侧最终 chartIndex 不同；若两面已在同一个Join闭包则不可满足。跨材质Join不可满足。非平凡Join组件的面并集须为单个拓扑圆盘；非圆盘可拒绝。支持任意正常弯曲的三角圆盘，包括不能无翻转投影到一个平面的圆盘，不能仅靠锁边或禁止LSCM回退来宣称支持。组件内部不因 maxCost/maxChartArea/maxBoundaryLength 软条件拆开，组件间候选接纳仍参考现有代价。可以采用保组件的替代参数化来恢复有效UV，不要求复现旧LSCM解。

不得简单将所有Join组件各自一岛或每个自由面各自一岛：约束为空时复用原ComputeCharts行为；默认选项下，无Cut的共面连通矩形条带一图表；从一条贯穿宽度的Cut分开的条带，两侧各一图表，其余自由边应合并。一般曲面无需固定最优图表数。每个输出chart必须面连通，每面一次；各chart内非邻三角内部无交叠、UV面积非零且绕序一致，邻边接触不算重叠。输入处于普通模型尺度，UV检查容差1e-5像素；不要求修复近乎退化输入。

compute 失败返回 false，之前成功的图表及 packed arrays 保持值和可用性，旧 PackCharts 还能重打包；先在候选内部完成再发布。合法后续setter与compute不受失败污染。成功后旧 PackCharts 负责常规页/图像生成，不自己另建不兼容输出。原角来源、全局chart编号、材质和页归属必须一致；约束改变后重算始终从原始网格开始。

固定 C/C++ 消费者只核对新符号及无硬边的最小正常结果，完整约束行为由新增三组测试负责。所有测试以 check.h 注册具名回调并实际使用 TASK_CHECK，不定义main。必需示例必须真实运行新入口并产生含非空polygon/path的SVG，脚本清旧产物，缺示例不可跳过。
