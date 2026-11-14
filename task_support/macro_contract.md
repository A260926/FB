# 用户图形宏支持范围

新增 C++ Decoder::SetUserMacroEnabled(bool enabled)；C 函数 void aribcc_decoder_set_user_macro_enabled(aribcc_decoder_t* decoder, bool enabled)，遵循已有C API参数/导出风格。Initialize后缺省关闭。启用是扩展 STD-B24 图形宏支持，TR-B14默认路径不改。

功能仅作用于 kARIB_STD_B24_JIS 编码。UTF8/Latin保留旧解释，不把其正文当作本功能。开关可在正常Initialize后设置。Flush清空用户表、清掉活动调用状态但保留开关；再次Initialize清表并恢复关闭；实际活动编码发生变化也清表但保留开关；SetProfile、相同编码重复设置和CS不清表。关闭开关清表，再开启不能恢复旧定义。每个实例独立。CS仍按原行为重置字集、颜色、坐标等显示状态，宏定义单独保留。

JIS正文控制字节95 40 MC定义、95 41 MC定义并执行、95 4f结束。MC合法范围21..7e，共94槽。95 4f显式结束时消耗这两个字节；新的95 40/41隐式结束当前定义，先提交当前项，再从新指令起点开始下一定义，不重复吞掉或跳过新指令。定义体因此不包括定义命令；data unit结束不是隐式终止符。缺MC、编号不合法、95后不足一字节、未知MACRO参数或缺终止符，返回错误，尚未完成的项不覆盖旧定义。没有上下文的95 4f为错误。完整项提交后的其他独立错误不撤销它。

定义体保存原始字节且不解释字符、ESC参数或样式。空体合法；所有权不依赖调用缓冲。定义后的立即执行在定义提交后进行。调用依据图形集kMacro进行，不局限默认G3/SS3：GL、GR、SS2、SS3都遵循既有designation/invocation。GR码按已有逻辑去高位后查号。没有自定义的60..6f仍执行原kDefaultMacros；其他未定义编号为空操作。有自定义的空体覆盖默认项。

执行语义与同位置的正文相同：调用前的字集和显示状态进入宏，宏产生的字、DRCS引用、颜色/位置变化进入当前Caption，状态变化传给后续正文；不替调用者恢复一份旧字集。外层字节消费只计算图形调用自身，宏独立的正文边界不能借外层后续字节补全残缺字符。允许间接调用；仅当将要进入的编号已经在当前活动链中时作为循环报错，不用全局visited禁止合法的顺序重用。错误后活动链清空，后续合法Decode仍可用已完整定义项。定义命令不允许出现在正在执行的宏体中。

启用JIS模式时，正文解析错误必须从ParseStatementBody/ParseDataUnit传回Decode，返回kError且本次out_result.caption为空；不要求回滚先前成功执行的显示状态或完整定义。关闭时保留原解析/默认宏行为。Caption与C API结果的所有权沿用现有接口。

依据：ARIB STD-B24 Version 5.2-E1 Volume 1 §7.1.1.6和Table 7-16（PDF页111）。本契约明确实例生命周期、循环和错误边界，避免把这些实现边界误称为标准逐字要求。官方来源：https://www.arib.or.jp/english/html/overview/doc/6-STD-B24v5_2-1p3-E1.pdf 。无需运行时下载标准文件。

test/decode/user_macro.cpp 使用 task_support/check.h；三组 definitions/execution/lifecycle 每组至少一个真实断言用例。task_support/pes.hpp 仅提供普通PES封装，不提供宏编解码实现。新增生产.cpp需接入上游CMake库目标，target始终重建并链接当前库，不能只在新测试里解释宏。
