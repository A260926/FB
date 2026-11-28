# 可暂停节点批遍历

C++17，新增wavefront.h/.cpp，namespace tinybvh。头包含tiny_bvh.h。公开：

```cpp
enum class WaveQuery { Nearest, Any };
enum class WaveState { Pending, Miss, Hit };
struct WaveResult { WaveState state; Ray ray; bool occluded; };
struct WaveStats {
 uint64_t nodeLoads,boxTests,triangleTests,completed,peakTasks;
};
class WaveBatch {
public:
 WaveBatch(const BVH&,const std::vector<Ray>&,const std::vector<WaveQuery>&);
 ~WaveBatch();
 size_t Append(const std::vector<Ray>&,const std::vector<WaveQuery>&);
 size_t Step(size_t budget);
 bool Done() const;
 const std::vector<WaveResult>& Results() const;
 WaveStats Stats() const;
 // 内部表示自由；不要求可复制/移动
};
```

输入支持float普通二叉三角BLAS，以Build/BuildQuick的object split结果为准，包括indexed和Slice stride。不支持BuildHQ空间切分、TLAS、自定义图元、wide布局、GPU和double新增批接口。构造可拒绝可识别的未支持布局，不扫描全树认证。未构建的空BVH合法。树/顶点/索引/opacity map在批次期间只读且存活；调用方射线和query数组可立即销毁/改写，批次自己保存副本。多个批次交错推进，同一批次串行调用，不要求并发。

尺寸不同、未知query枚举、非有限O/D、D全零，或hit.t非正/非有限时构造抛invalid_argument。调用方保证rD按库约定初始化；O+tD的D不要求单位化，方向正/负零分量与盒边界要正确处理。构造必须完整验证射线再建立任务；无修改外部数据。Append使用同样的完整预检，失败不改变会话、结果或统计；返回新批第一条在全会话结果中的下标，结果永久按追加顺序排列，空追加只返回已有长度且不改变状态。允许在任意Step之间以及Done=true后追加，追加的有效射线进入Pending，Done重新按当前工作决定。先前完成结果保持不变，completed累计；无需回收旧结果槽或ID。射线hit.t为半开上界，几何命中区间[0,hit.t)。已有普通单射线的三角/opacity数值规则不变；新增路径必须复用原生产三角求交和opacity判定，可添加窄内部适配器，不另写第二份求交算法。测试避开opacity微格精确边界，未扩展其分界约定。

Results长度等于输入，始终按原下标，不能用队列压实槽号代替身份。初始Pending，ray保存完整输入；非空树工作未结束时不提前交付Nearest的局部候选。Nearest完成后，真实最近命中为Hit且ray.hit返回生产命中t/u/v/prim；没有命中为Miss并逐字段保留输入hit，occluded=false。Any首次有效遮挡即Hit且occluded=true，其ray保持输入逐字段不变，Miss的occluded=false。两种查询都保留O/D/rD/mask/instIdx及非hit用户字段。仅比较被初始化的字段，不读取Intersection未初始化的填充。精确等距最近面可任选其中之一，但同批不同预算切分必须选同一面。唯一最近面与原Intersect一致；所有遮挡与原IsOccluded一致。

队列按node-major组织：同一原生节点的所有活跃ray lane（允许多个64位掩码字）是一项任务，不是每条射线一个遍历栈。子任务只由父任务派生，并保存被预算切断的位置。晚到lane可与已有同node任务合流；如果该node的旧cohort已经走到叶中间，新lane仍须从该叶首图元补算，旧lane不得重做已做的测试。可用不同进度cohort表示直到游标追平再合并，不能简单OR新mask后让新lane继承旧游标。无需把整个队列按全树重建。会话按需保存已经读过的节点元数据，同一个原生节点在整个会话（包括多次Append）只从原树加载一次，第一次加载时nodeLoads加1，之后从会话缓存取元数据不计加载。缓存只能按实际访问建立，不预扫树。内节点每个活跃lane对每个孩子的盒测试各计boxTests一次；叶里每个活跃lane/原图元测试计triangleTests一次，无论命中/拒绝。使用实际原树bvhNode/primIdx；不预扫树、不复制树、不清零节点数乘射线数的矩阵，不按ray调用完整Intersect/IsOccluded/Bundle。允许O(全部已提交ray数+实际访问节点缓存+当前frontier掩码/游标)内存，不能隐藏每ray栈。

Step最多执行budget个上述操作，返回本次nodeLoads+boxTests+triangleTests增量，必须与统计之和吻合；budget=0完全不改状态/统计。空任务跳过、掩码更新和结果结算不计操作，但正预算且还有工作时必须能进展。node读取、内节点某个lane的第二个孩子盒测试、叶子某个图元的中间lane，都能作为暂停位置，不能为了凑预算把整叶先算完。没有操作后续可做时结算所有剩余结果，Done为true；完成后且未追加时Step任意预算返回0且无变化。Append只验证输入/登记根工作，不能提前求交或加载节点；node/box/triangle计数在Append前后不变。构造除根任务及输入验证外不做计数操作，初始nodeLoads/boxTests/triangleTests为0；空树/空射线直接完成，completed为输入长度。

Any lane一旦命中，所有队列中的该lane都停止：之后不能再为它做盒/三角测试；空掩码任务不加载节点。Nearest每次盒测试使用当前该lane上界，opacity拒绝不提交或缩短上界。执行顺序固定，Step切分不能改决定；相同初始批次用预算1、7、交错预算及一次大预算必须有相同结果和最终全部统计（含peakTasks）。有在线追加的比较要在相同已执行操作数的位置提交相同追加，因此外部事件序列一致；可将一次追加置于叶中、一次置于Done之后。不同外部追加时刻允许改变工作量。peakTasks为队列同时保留的非空节点任务数最大值，completed为已结算lane数。预算不能通过未计数预处理或先跑全查询再慢慢发布结果绕过。

原生产入口的集成：float普通BVH::IntersectBundle/IsOccludedBundle新增使用本批内核的分派（固定bundle长度仍按原宏）；原TLAS分派继续保留。没有本机向量化实现时，float BVH4_CPU的这两个bundle回退也通过它持有的原生binary BVH调用同一批内核。double旧回退不变，有本机向量化的wide bundle保留原路径。不得关闭SIMD、改变普通单射线或把所有布局退化为标量。可将核心状态放在独立生产模块、以窄适配器连接现有模板实现。

共享静态packet与独立窗口不是新增能力；新增核心是在线cohort与叶中游标合流、操作预算及节点缓存的生命周期。三组新增测试masks/resume/opacity使用task_support/check.h计数驱动。除独立三角参照，还要对既有入口对照和真实标量bundle新分派验证。对129条完全相同射线，同一树节点只加载一次；nodeLoads应与同样查询类型的单lane批相同，而boxTests/triangleTests要对应实际lane工作。用叶中Any早停材料核后续工作减少，用互不相干远簇核真正盒剔除，禁止靠特判坐标/数量。测试还需构造有至少三个图元的单叶或小叶：旧lane已经测过首图元但尚未完成时追加一个只命中首图元的新lane，核新lane正确而旧lane测试数不增加；Done后追加重复射线必须正确且不再加载此前见过的node，新增box/triangle测试仍真实计数。原Build/Slice/index和micro map能力已有，不算新能力。

示例examples/wavefront.cpp仅接输出文件，用真实主命中批和有限阴影批绘制，至少32x32 ASCII P3 PPM。测试须检查非背景颜色及阴影关系，而非只看文件。固定包装在全新自有build子目录中分别编译scalar/native新头源/接口消费者/三组测试/示例；清除示例输出后再运行并解析尺寸/数据范围。每份脚本中的生产头wavefront.h必须强制-include；生产源wavefront.cpp独立非空编译并链接。不得用预置目标实现或伪造测试成功替代。
