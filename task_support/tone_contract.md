接口与数值约定

新增 tone_map.h/.cpp，namespace tone。Frame { int width=0,height=0; std::vector<std::array<double,3>> rgb; }，按从上到下行序排列。
Options { double alpha=0.1,beta=0.85,key=0.18,tolerance=1e-8; int max_iterations=2000; }。
Result { Frame linear; std::vector<double> log_luminance,attenuation; double relative_residual=0; int iterations=0; }。
Result compress(const Frame&, const Options&)。
RenderHelp.h 公开 void BeginHDR(const Vec3f& background) 与 tone::Frame EndHDR()。
重复Begin或无Begin的End抛std::logic_error且不改状态；Clear/Init/Reset取消捕获。HDR拥有单独的RGB与rhw深度，Begin清成背景/0；普通Bitmap和深度不被HDR绘制修改，End仅取出快照。HDR内线框模式不支持，DrawPrimitive在改任何像素前抛logic_error。End之后回普通绘制。RGB有限非负，PS alpha有限，<=0丢弃，其余不透明，仍按原rhw较大者胜出/相等后来者覆盖。VS/PS调用、半开覆盖与插值继续走原路径。

Frame尺寸必须正且乘积与rgb.size相等，所有分量有限非负；alpha∈(0,1]、beta∈(0,1]、key>0、tolerance∈(0,1)、max_iterations>0且浮点参数有限，否则抛std::invalid_argument。允许1×1、单行、单列。
Y=.2126R+.7152G+.0722B，h=log(max(Y,1e-6))。第0层就是h。每层宽高各为ceil(n/2)，新像素是对应上层有效2×2子块的平均，不补零不复制边界；降至1×1结束。第l层前向差分dx=(h[x+1,y]-h[x,y])/2^l，dy同理，缺邻居取0；g=sqrt(dx²+dy²)，m是本层所有g的平均。若m=0或g=0，phi_l=1；否则phi_l=min(1,(max(g,1e-12)/(alpha*m))^(beta-1))。每层phi独立按归一化像素中心双线性clamp采样到原尺寸，最终attenuation是这些上采样值的乘积。具体坐标：原图中心(x+.5)/W映射层纹理坐标u=(x+.5)*Wl/W-.5，再夹到[0,Wl-1]后作相邻中心插值；y同理。

全图每条向右/向下的真实内边p->q，目标梯度Gpq=(hq-hp)*(phi_p+phi_q)/2。最小化sum_edges((xq-xp)-Gpq)^2且sum(x)=0；图外不造边，不环绕，不加屏蔽项。log_luminance返回这个x。令(Lx)p=sum_neighbors(xp-xq)，bp为相同方向规则的目标散度（边p->q对bp贡献-Gpq，对bq贡献+Gpq）。relative_residual=||Lx-b||2/max(||b||2,1e-15)，须从返回的x重算。无边/零b取x=0、残差0、迭代0。O(W*H)存储，不能构造稠密生产矩阵；max_iterations用尽未到tolerance抛std::runtime_error，不返回部分成功。结果中x均值绝对值应<=1e-10，实际残差<=tolerance。

逐像素Yd=key*exp(x)。原Y>0时C=原RGB*(Yd/原Y)，原Y=0取C=0；linear.rgb返回逐通道C/(1+C)，不做gamma和字节量化。例子PPM写出时再作标准sRGB分段变换及四舍五入。constant灰图的输出为key/(1+key)，beta=1时重建h减均值；源亮度整体缩放、且所有Y都在floor以上时结果不变。小图结果与独立系统比较，x和attenuation采用1e-7绝对+1e-6相对误差。禁止修改源Frame。
