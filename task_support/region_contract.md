本次新增的公开能力是 REGION 区域定义、cue 引用和滚动区域播放器；头文件仅声明接口，未提供实现。所有新增生产代码、测试与示例使用 C。原有 C++ 包装层及测试继续使用。

解析与对象

- 从原有 parse_chunk/finish_parsing 入口接受数据，不能改为等全文到齐后交给另一套解析器。REGION 独占一行，位于签名后的空行与首条 cue 之间；后续各行是 key:value，空行结束。仅支持新版块语法，不要求旧 Region: 签名元数据。区域回调可在首次 parse_chunk 前用 webvtt_parser_set_region_callback 注册，NULL 表示关闭；沿用创建 parser 时的 userdata。回调是借用，需跨回调保留就 ref_region，最后 release_region。未注册回调不影响 cue 绑定。
- 初值：width=100、lines=3、regionanchor=0%,100%、viewportanchor=0%,100%、scroll_up=false。百分比接受普通非负十进制小数并要求尾随 %，在 [0,100]；锚点是逗号分隔的一对百分比。lines 是正十进制整数且不超过 webvtt_uint 的范围；scroll 只接受 up，未知值忽略。id 非空且不含空白或 -->，保持 UTF-8 原字节；同块重复属性取最后一个合法值。未知字段忽略，已知字段非法则保留先前值并报一次可恢复错误（可新增区域错误码），错误行列指向该属性行开头。缺合法 id 的块丢弃并在块末报告错误。
- 完整块结束后才发布不可变区域和更新本 parser 的查找表。同名块可连续出现，最后完成的版本用于后续 cue；之前回调保留的版本不得变。REGION 块不能成为 cue。第一条 cue 之后出现 REGION 块时整块忽略并报可恢复错误，到下一空行恢复普通 cue；普通正文内的 REGION 字样仍是文字。非法区域字段不丢弃整个正常后续 cue。
- 分块不能改变结果和发布时机。至少支持 LF、CRLF，CR 与 LF 可分处两个调用；定义块后的空行消费完毕就应发布，不等 finish。EOF 可结束末个 REGION 块或末条 cue，重复 finish 不重复发布、交付或报告区域错误。已有普通字幕的错误位置与所有权约定保持。
- 给 webvtt_cue 加 struct webvtt_region_t *region（NULL 表示未绑定）。region:ID 取最后一个语法合法的该设置；在时间行全部设置处理后查找区域。未知 ID 不绑定。有效设置最终若 vertical 非 horizontal、line 非 auto 或 size 非100，则也不绑定，不因设置顺序而改变。缺省及 position/align 不阻止区域绑定。原有 webvtt_cue_set_settings 没有 parser 上下文，可存引用名供随后解析器绑定，但不得访问全局区域表。
- 表持有区域引用，cue 另持有引用；删除 parser 或替换表项仅释放自身引用。回调的 cue 所有权仍转给调用方；cue 最后释放时同步释放 region。区域字符串和内容不能借用输入缓冲。解析器中途删除也清理未完成定义，不共享跨实例状态。

播放与行队列

- create 接受任意输入序 cue 数组（count=0 可传 NULL），保留所有有效 cue 的引用，记录原输入下标。不转移调用方所有权；创建失败清理已经取得的引用。播放时间为非负毫秒；创建后尚未处理任何事件。advance(t) 处理到 t（含），已推进后传更小时间返回 INVALID_PARAM 且不改变原帧；seek(t) 清空本播放器显示状态并重放至 t。重复同刻查询不重复加入。调用方不得并发修改正在播放的 cue。
- 只显示绑定 scroll_up=true 区域的 cue。事件按 from 再按输入下标排序，[from,until) 为寿命。每个事件时刻先删除所有 until<=该时刻的在列行，再按次序加入 from 等于该时刻的新 cue；推进末尾再移除 until<=t 的行。跨过一条字幕的完整寿命仍处理其到达及影响。无绑定/不滚动的 cue 不进入区域行队列，普通解析回调仍正常交付。
- 从 node_head 依文档序收集 TEXT，跳过 RUBY_TEXT 子树及 TIME_STAMP，不输出标签，保留换行；空正文无行，其余按 \n 拆分并保留尾空行。每行保存 cue 原下标和零基 line_index。新行依次追加；区域超 lines 时从最旧行逐行挤出，允许同 cue 只剩后半段。到期仅删除队列中该 cue 幸存行，剩下行保持顺序并靠底排列。挤出记录不因别的 cue 到期而撤销；seek 通过重放同一历史重建，不能重新只取当时活跃 cue。
- rows 返回借用数组及其长度，区域顺序按该区域在输入 cue 数组中首次出现的位置，区域内从上至下。text 是以 NUL 结束的 UTF-8，每行没有换行符。数组及字符串有效至下一次 advance/seek/delete，读取和 SVG 写出不改变状态。两个 player 不共享游标、行队列或缓存；cue、区域、节点树不被写入播放器状态。

SVG 与命令

- width、height、line_step 必须是正有限数。区域屏幕宽 W=width*region.width/100，高 H=lines*line_step。左上角 x=width*viewport_anchor_x/100-W*region_anchor_x/100，y=height*viewport_anchor_y/100-H*region_anchor_y/100。不夹紧锚点结果，以该区域矩形作 clipPath，再由 SVG 视口裁切画外部分。
- 按 rows 的区域顺序输出分组，data-region 存 id；同名不同对象不合并，各自 clipPath 使用无冲突编号。矩形使用上述 x,y,W,H。区域内共 n 条行，第 i 条（零基）的 text 基线是 x、y+(lines-n+i+1)*line_step，带 data-cue-index、data-line-index。空区域不输出分组；空帧仍为合法 SVG。不做字体度量、自动折行、动画过渡、普通 cue 布局或 CSS 样式；这些不应成为新增的隐含工作。
- region_player_write_svg 输出一个完整 SVG。text 和属性分别正确转义 &,<,>,引号；数值按 C locale 写出。examples/vttregions.c 读取普通本地文件，用原有 chunk 入口喂入并通过 callback 收集 cue，finish 后删除 parser，再创建 player 并释放收集器自有 cue 引用。参数中的第一个时间也从初始状态播放；后续时间减少时 seek，否则 advance。每帧前输出 <!-- frame TIME -->，stdout 仅包含这些注释及 SVG，错误写 stderr 且非零。

验证入口

新增 tests/region_player_test.c（注册五组 task_case、使用 TASK_CHECK），以及 examples/vttregions.c。task_support/run.sh target 会真实编译这两个文件，并在进程外检查所有分组的非零 case/check 及最终完成记录，随后运行正常解析基线和所选旧设置测试。TASK_CHECK 失败会使 case 立即返回非零；每例返回0表示成功。不能自行实现 main，driver.c 提供 main。保留现有测试；这份契约及基线不包含区域/播放器目标答案。
