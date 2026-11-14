# SVG 字幕接口

公开 renderSVG(isd, {width,height,measure})，同步返回带 xmlns、width、height、viewBox 的完整 SVG。width/height 为正有限像素值；measure(text, style) 返回 {width,ascent,descent}，均为有限非负数，非空文字的 ascent+descent>0。style 包含数值 fontSize 和 fontFamily/fontWeight/fontStyle，来自当前 ISD 的计算后样式；不得用字符串长度代替测量。错误输入或测量结果抛异常。SVG 入口和公开 main.js 在无 DOM 的 Node 中可导入。不得修改 ISD 或借用全局布局状态。

只支持 lrtb 横排、普通段落/嵌套 span/br、颜色/字体/opacity、区域 origin/extent/padding、displayAlign、段落 textAlign、wrapOption、lineHeight，以及简单 ruby。ISD 中的 ComputedLength 按画幅 width/height 求像素值，不按区域尺寸再次缩放。区域 padding 从内容区扣除；段落按文档顺序排，无额外段间距。overflow=hidden 时用 SVG clipPath 裁到区域矩形，重复调用生成的每份独立 SVG 内 ID 不冲突。区域背景色按区域矩形输出。

默认空白跨 span 合并成一个 U+0020；行首尾空格丢弃。xml:space=preserve 的空格不折叠且保留，仍允许在空格后折行。br 强制换行并保留空行。自动折行只在空格处分开词组或在 ruby 原子组前后；同一个词跨多个样式 span 仍不可拆。行容不下单个词/ruby 时单独成行，允许溢出后由区域裁剪；noWrap 只响应 br。断行空格不进入行宽。每个文字片段保留自身字体和颜色；不支持的 vertical/bidi/image、嵌套或非成对 ruby 要明确抛异常，不能悄悄丢字。

ruby container 内一个 base 与一个 text，可以各有普通样式 span，delimiter 不显示。两层各按自身测量宽度求和，占宽为两者最大值，各自水平居中，不拆组。rubyPosition=before/after 表示注音在底文上/下，不做 outside。每行底文 ascent/descent 分别取普通文字和 ruby 底文的最大值。核心行高取 ascent+descent 与本行显式 lineHeight 像素值的最大值，normal 不加额外行距；多余行距上下各半。before/after 各预留本行该方向注音 ascent+descent 的最大值（没有则0）。行高是前预留+核心行高+后预留。注音 before 的底边贴核心行顶边，after 的顶边贴核心行底边；每层内部共用自身最大 ascent 的基线。空行核心高度用段落字体测量 M 后的 ascent+descent 与显式 lineHeight 的最大值。

完成所有行后，用整块最终高度做区域 displayAlign：before 从内容区顶部开始；center 偏移 max(0,(内容高-块高)/2)；after 偏移 max(0,内容高-块高)。每行 textAlign=start/center/end 按最终占宽对齐。输出真实 SVG text/tspan，给出可检查的数值坐标与字体/颜色/透明度，正确转义文字和属性。禁止 foreignObject、脚本、外部 CSS/字体加载、浏览器 API。描边、阴影、复杂字形塑形不在此接口范围，测量者负责字体度量。

新增 examples/ttml-svg.mjs，参数 input.ttml seconds width height，读取 TTML 经现有解析、generateISD、renderSVG 输出 stdout；示例可用明确标注的固定字宽测量函数，不能把它塞进库内。失败非零退出。新回归放 tests/svg.test.mjs，使用 task_support/check.mjs 的 taskCase/check，三组名称见请求。基础材料不含新排版实现或新回归答案。
