输入尺寸为1..2048的整数，时间有限；画幅按 width/PlayResX、height/PlayResY 从脚本坐标缩放。缺少正 PlayRes 拒绝。透明初始帧；输出不共享输入存储。

路径使用现有 instructions，不另解析 ASS。每条有效对白允许多个 drawing fragment，所有片段叠在同一个局部原点，不作文字 advance。p>=1 时绘图坐标先除以 2^(p-1)；pbo 在该缩放之后加到局部 y。每个片段用本 slice 样式和 fragment.tag 合并得到静态值，reset 已由 slices 表示。取所有片段缩放后控制点范围的并集作对白盒（与上游 drawing 的控制点外包围一致），按 an1..9 的左/中/右、下/中/上选锚点并移至局部原点；再按 fscx/fscy 百分比缩放。无 pos/move 时锚点位置由脚本画幅、margin 与 alignment 决定，左右使用对应边距，居中用可用横向区间中心，上下使用vertical边距、中间用画幅中线。frz 按屏幕坐标顺时针旋转，默认以锚点为中心；org 存在则以脚本坐标 org 为中心。先按 pos/move 放置，再旋转，最后画幅缩放。负坐标正常裁掉。

move 的 t1=t2=0 表示整条对白时长，其他相等端点为阶跃；区间外钳制。fad 的两个值为淡入/淡出毫秒，因子分别为 clamp(local/tin)、clamp((duration-local)/tout)，零时长因子1，取两者最小。fade 的三个alpha是透明度字节，四个时刻顺序非递减：段外用首尾值，中间两段线性插值，平台用中值，零长段按右值阶跃；转换成不透明度因子。主色是BBGGRR，不透明度为 (1-a1/255)*fade因子，保持浮点到最终输出。支持范围外非零 bord/shad、blur、frx/fry、fax/fay、t 和非空文字均抛错；其他非绘制属性可忽略。

像素(x,y)固定16个子样本，脚本像素内位置 ((i+0.5)/4,(j+0.5)/4)，i,j=0..3。多轮廓按非零绕数填充；每个M开始的子路径自动闭合，边上判定采用向右射线与半开y交叉，交点x严格大于样本x才计数。三次曲线在输出坐标中逼近，折线与曲线的最大偏差不超过1/64像素；无需固定细分数。

矩形clip为[x1,x2)×[y1,y2)，绘图clip按自己的scale独立缩放，均在脚本坐标，不应用对白的p/pos/move/frz；iclip取补集。每个样本同时满足绘图覆盖和clip才绘制；同片段子路径一次填充，不按轮廓重复叠色。对白按layer升序，同layer按document.dialogues原次序；对白内部按slice和fragment顺序，每样本用非量化预乘RGB/alpha作source-over。最后平均16样本的预乘RGB和alpha，再除以平均alpha，逐字节Math.round并钳制0..255；alpha为零则RGB全零。不做gamma变换。

CLI以Node读取input.ass，输出P7头：WIDTH、HEIGHT、DEPTH 4、MAXVAL 255、TUPLTYPE RGB_ALPHA、ENDHDR后紧跟RGBA字节。CLI用脚本PlayRes作为输出尺寸。写出失败必须非零退出。类型声明保留旧API。
