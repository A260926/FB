# Function-reference flow

这是一个只读抽象分析接口。所有输入都来自合法合成模块；不实例化、运行或解释执行 Wasm，不调用导入，也不读取进程内存。数值是无值信息的标量，占据原本的栈槽；不按常数条件删分支。

在 include/wabt/reference-flow.h 和 src/reference-flow.cc 新增以下公开入口，接进原 wabt CMake 目标。采用项目的 Result/Errors：

```cpp
Result BuildReferenceFlow(const Module&, const std::vector<Index>& roots,
                          ReferenceFlow* out, Errors* errors);
```

分析对象是经 ResolveNames/ValidateModule 后、变量索引已解析的 Module。二进制路径必须用 ReadBinaryIr 再 ValidateModule。开启 function_references。函数索引包含函数导入前缀，其他导入不占位。支持的函数类型不含 GC、递归类型组或子类型声明；同结构签名按参数/结果和引用可空性比较，不用 type 编号代替类型。

公开结构如下，可增加辅助字段；Index 使用 wabt::Index：

```cpp
struct RefValue {
  std::vector<Index> functions;
  bool may_be_null = false;
  bool external = false;
};
struct FlowCall {
  size_t expr_ordinal;
  bool indirect;
  std::vector<Index> targets;
  std::vector<size_t> callee_contexts;
  bool external;
  bool may_trap_null;
};
struct FlowContext {
  Index function;
  std::vector<RefValue> arguments;
  bool may_return;
  std::vector<RefValue> results;
  std::vector<FlowCall> calls;
};
struct ReferenceFlow {
  std::vector<size_t> roots;
  std::vector<FlowContext> contexts;
};
```

functions 是排序去重的完整函数索引集合；external 表示还可能有模块外函数引用，和已知模块内集合并存。nullable 是单独的可能性，不能把 null 当函数 0。空集合、无 null、无 external 是 bottom，没有任何已知正常值。每个 arguments 只包含函数引用参数，按原参数顺序；数值参数不参与上下文键。results 则与完整结果位置一一对应，标量位置填空 RefValue。

同一函数、同一引用实参元组共享一个上下文；不同元组独立。每个 root 的引用参数取 top：所有与其声明引用类型兼容的模块函数，加 external，声明可空时加 null。导入函数假定可能正常返回，引用结果同样取 top；导入不会执行。已知 ref.func 导入仍用原函数索引报告，不用 external 冒充该身份。根与直接调用都可以指向导入。

本地函数初始 may_return=false、结果 bottom。正常落到函数尾或 return 才产生正常返回，汇总所有正常返回路径。void 函数也要有 may_return，不能因没有引用结果误判。直接调用和 call_ref 都按声明顺序消耗完整参数；call_ref 先取栈顶函数引用，再取调用参数。对具体候选分别带着当前实参创建/查询上下文，只有 may_return 的候选才为后续传播返回值，按结果位置合并；未知外部候选使用导入式 top 结果。新上下文、摘要结果和正常返回性增长都要让依赖者重新计算，直到最小不动点。不能设置递归深度截断，也不能把递归环直接放大为 top。

call_ref 的 null 分量产生 may_trap_null，但不产生 callee。null-only 的调用没有正常续点；bottom 的调用也不能先当作 top 继续扫描并永久留下虚假调用边。target 候选、callsite 和上下文关系应由收敛状态重新确定。纯递归环没有正常返回；有真实返回臂的互递归应把引用结果传回最早的调用者。return_call/return_call_ref 的结果直接进入当前函数摘要，之后的语句没有正常续点。trap 只建模显式 unreachable 和已知 null-only 的函数引用调用，其余数值操作可能的 trap 不求值也不删路径。

local.get/set/tee 按当前状态传递引用；set/tee 对该局部变量强更新，不能总是并集。可空引用局部初始为 null，验证允许的非空引用局部在赋值前为 bottom。block 和 if 使用真实块签名，包括多值参数/结果；if 的两臂从同一输入栈与 locals 快照分别开始，仅正常续行的两臂合并。已 return、无返回调用和 unreachable 的一臂不参加尾部状态合并。if 条件及 select 条件未知，不能用常数推断。select 合并两个候选引用；ref.as_non_null 删除 null 分量，删完为空则无正常续点；ref.is_null 只产出标量。

支持 const、nop、drop、select/typed select、local.get/set/tee、ref.func/null/is_null/as_non_null、标量 unary/binary/compare/convert、block/if、return/unreachable、call/call_ref/return_call/return_call_ref。标量操作按现有 Opcode 签名维护槽，不恢复数值。loop、br/br_if/br_table、table、memory、global、EH、SIMD 及其他未列出的函数体节点报 unsupported，不以未知正常结果跳过。模块里的内存导入、函数声明和 declarative elem 可以存在，不做装载。只需分析从 roots 发现的函数；发现某本地函数时检查其完整语法体的支持范围，死区也不静默接受未支持节点。无须为未到达函数添加上下文。

每个函数体的节点按固定深度优先前序编号，从 0 开始，控制节点先编号，if 先 true 后 false，else/end 不占号；死区的节点仍占号。这是 expr_ordinal，不能以每轮扫描计数或 IR 指针地址作身份。callsite 即使只有 null 也保留一条记录，之后的不可达调用点不发布。targets 包含具体模块候选，external 对应未知外部候选，callee_contexts 指向目标本地/已知导入上下文。上下文按函数索引、引用实参元组字典序排序（集合序列后比较 null/external），重新映射所有索引，calls 按 ordinal 排序；roots 排序去重。收敛后只保留从最终 roots/callee_contexts 可达的上下文，清除仅在中途实参集合较小时出现、后来已被替代的上下文；不依赖根输入顺序。

无效 root、超范围索引和不支持节点以 Result::Error 和 Errors 返回；消息含完整函数索引、expr ordinal，能够取得源 Location 时一并保留。失败不发布半张报告，原 out 保持。此入口不替代通用 Wasm 验证器。报告拥有全部集合和关系，不借用 Module、输入字节或字符串视图；释放输入后仍可读取。重复/交错分析两个模块不串缓存，不改原 Module。

在 src/tools/wasm-ref-flow.cc 实现 .task-build/wabt/wasm-ref-flow FILE --root N [--root N ...]。读取、验证和调用库，失败非零并输出诊断。使用确定性的逐行 JSON 输出，每个 context 一行，字段包含 context（排序后下标）、function、arguments、may_return、results、calls；调用项含上述所有字段；末行输出 roots。测试负责解析并比较具体内容，不只搜标题。调用工具只做静态分析，不能借用 wasm-interp 等执行工具。

新增 test/task_reference_flow.cc 无 main，通过 task_support/check.h 注册 contexts、locals、recursion、consumer 四组。每组真实 TASK_CHECK 内容比较并返回0。fixture.h 仅提供原始 WAT→解析/验证→写出→ReadBinaryIr→验证的辅助入口，不提供新分析答案。可扩充 reference-inputs.wat，也要有独立小输入和手算候选。contexts 验证同helper不同实参、引用返回再call_ref、导入前缀及top；locals 验证强更新、if快照/仅正常臂合并、select、多结果对应；recursion 联合覆盖互递归有出口、纯递归、null-only、tail调用和二次分析；consumer 真正启动新CLI，比较库报告、重复/逆序根及失败输出，原二进制写回仍相同。保留输入释放后结果可读的对照，增加 docs/reference-flow.md 说明抽象精度与限制。

运行 bash task_support/run.sh baseline 是原始生产链；target 会重编 wabt、测试和存在的新消费者，再执行四组及旧基线。task_support/CMakeLists.txt 将 src 根新 .cc 一并加入实际库，并识别 src/tools/wasm-ref-flow.cc；仍请把生产源列入原 CMakeLists.txt，便于普通库构建。
