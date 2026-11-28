公开接口（C++17，hxa7241_minilight）

新增 src/Coherent.hpp/.cpp。SpectralLine { double wavelength=1; Vector3f sensor; }；OpticalKind枚举Absorb, Mirror, Splitter, Source。
OpticalSurface { OpticalKind kind=OpticalKind::Absorb; double reflectance=1,phase=0; int coherence_group=0; std::vector<double> intensity; }。
CoherentConfig { std::vector<SpectralLine> lines; std::vector<OpticalSurface> surfaces; unsigned max_events=12; std::size_t max_states=100000; }。
CoherentResult { Vector3f radiance; std::map<int,std::vector<std::complex<double>>> fields; std::size_t queries=0,terminal_paths=0,truncated_paths=0; }。
CoherentTracer(const Scene&, const CoherentConfig&)，以及 const trace(const Vector3f& origin,const Vector3f& direction) 返回CoherentResult。语法为 CoherentResult trace(...) const。
Scene 增加 int32 getTriangleCount() const 和 const Triangle& getTriangle(int32 index) const，越界抛std::out_of_range。输入的每个三角形占一个位置，不因是否发光而变化。追踪器持有配置值副本，Scene必须活得更久且不改动；无需复制Scene。根据原getIntersection返回的三角指针识别材质，构建时建立映射，禁止每事件扫描整场景找序号。
Camera 增加 void getCoherentFrame(const CoherentTracer&, const Random&, Image&) const；Image增加 Vector3f getLinearPixel(int32 x,int32 y,int32 iterations) const，返回累计值/iterations，坐标是addToPixel的下方原点坐标，索引越界或iterations<=0抛invalid_argument。只给新入口加功能，旧入口不能改变。

模型和输入

这是互易、标量的相干几何光学预览，忽略衍射和光束横截面变化，不宣称完整波动光学。所有谱线波长使用世界长度单位，sensor是从这条谱线强度到线性RGB的非负响应。谱线数1..16，波长有限正，sensor有限非负。surfaces长度等于三角形数。Source的intensity长度等于谱线数，值为有限非负的辐射强度；非Source的intensity必须空。phase有限，reflectance对Mirror/Splitter必须在[0,1]。Source的coherence_group>=0，其余材质不使用该字段。max_events为1..32，max_states>=1。非法配置构造时抛invalid_argument；不改原配置。trace的方向有限且为单位，原点有限，测试不含退化三角、共面传播、完全等距的重叠三角或擦边命中。

传输

相机射线朝场景D。吸收面终止，未命中为黑。单面Source仅在dot(-D,N)>0时贡献，其场为sqrt(intensity[line])*exp(i*phase)，此后终止；不读取旧emitivity。Mirror将D变成D-2dot(D,N)N，复系数sqrt(reflectance)*exp(i*phase)，没有透射。Splitter同时发反射和原方向透射两支，复系数分别i*sqrt(reflectance)*exp(i*phase)及sqrt(1-reflectance)*exp(i*phase)，正反面用相同规则。零系数分支不生成。此2端口系数的反射相移不可丢失。

每个分支从上一个真实几何点发射，沿用原Scene::getIntersection的lastHit排除上一三角。不能将数值偏移加入光程。每一段长度是两个真实点的欧氏距离，传播乘exp(i*2pi*length/wavelength)。从源沿互易链回到传感器的全部系数相乘。一个分支携带全部谱线的复振幅；几何拓扑与谱线无关，不应按谱线重复Scene查询。可逐段约减相位或以等价方式控制精度，不要累加巨大角度再一次sin。测试通常光程/波长<=1e5，场误差1e-8绝对+1e-7相对；额外长程场<=1e10时允许1e-4相位误差。

每次trace的fields按源coherence_group聚合；每组vector长度=谱线数，单条终点路径先将携带的振幅乘该源场，再加到对应槽。源组相同才干涉，组间、不同谱线之间只加norm(field)。radiance=sum_groups,sum_lines sensor[line]*norm(fields[group][line])。不按单条路径平方，也不把各谱线先加振幅。没有源命中时fields为空。这里源之间的相干性由调用者显式指定，不根据三角形身份推测。

查询和预算

queries只在实际调用Scene::getIntersection处递增；多谱线共享它。terminal_paths是实际到达正面Source的几何分支数（全零源也计一次）。max_events按沿每条链已命中的表面数量计，Source作为第max_events个命中仍可贡献；Mirror/Splitter在该次数用尽时不再发出的每个非零分支计入truncated_paths。没命中/吸收/零系数不算截断。max_states是一次trace允许的总Scene查询数；即将超过时抛runtime_error，不返回部分结果。不能用极小振幅阈值、随机终止或随意丢分支满足预算。配置副本与临时复场、计数隔离，重复/交错trace不能累计旧结果。

相机与曝光

getCoherentFrame沿用Camera::getFrame的循环次序、视角、宽高比和每像素两个Random实数抖动；相机射线构造应抽为共用生产逻辑。相干trace不取Random。对同一相机样本，先完成所有谱线/光路合成强度，再调用Image::addToPixel；跨样本只加强度。先用Random副本完成新帧并暂存RGB，所有像素都成功后才把临时RGB加到Image并把随机流更新为副本的末状态。预算错误时原Image和传入Random逐字段保持原样，已有帧不被擦掉。重用原Image::getFormatted的强度均值与PPM色调映射，不把复场存到Image里。只要求单线程。新入口加帧两次，getLinearPixel(...,2)应给两帧强度平均。

示例必须用有限三角形搭出可见的两臂分束/重合光路，至少一个输出像素有两个非零Source终点贡献，两个场景只改变一个源phase或一个镜面phase，从相长变相消。实际调用CoherentTracer和getCoherentFrame；写标准P6，必须有中心线性值，不能预填解析干涉纹。README解释场景布局及漏出视野的另一端口。
