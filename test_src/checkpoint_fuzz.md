# checkpoint_fuzz 覆盖说明

`test_src/checkpoint_fuzz.c` 针对 sljit 编译器的序列化续接路径做结构化 fuzz。
关注点是一类特定的续接点：**算术指令（产生状态标志，含进位/借位）已经发射，
而第一个标志消费者（ADDC/SUBC 链、条件物化或条件跳转）尚未发射**。本文说明
参考模型如何区分 ADD 的进位与 SUB 的借位，以及为什么选定的续接点确实跨过了
被序列化的编译状态 `status_flags_state`。

## 1. 独立整数模型：进位与借位是两种不同的语义

模型全程只用无符号模运算，所有操作数先按目标位宽（32 或机器字宽）掩码，
中间和也按 `2^bits` 归约。不使用有符号 C 运算（因此不存在有符号溢出 UB），
不依赖更宽的整数类型，也不调用任何架构条件码或库的标志分类函数来求期望值。

- ADD：`r = (a + b) mod 2^n`，**进位 carry = (r < a)**。归约后的结果小于
  任一加数即说明越过了模数，这是无符号比较，不依赖把高位和右移出来。
- SUB：`r = (a - b) mod 2^n`，**借位 borrow = (a < b)**。被减数在无符号
  意义下小于减数即发生借位。
- 带进位/借位的后续运算各自有独立原语：
  - `mod_add_carry` 把上一跳的 carry 当作 1 加进去，再用
    `(t < a) || (cin && t == mask)` 求新的 carry；
  - `mod_sub_borrow` 把上一跳的 borrow 当作 1 减掉，再用
    `(a < b) || (bin && t == 0)` 求新的 borrow。

关键点：carry 和 borrow 不共用“和的最高位”这种不可靠来源。曾经的写法
`full = a + (mask-b) + c; full >> bits` 在同宽类型里会先回绕、丢掉真正的
高位，导致借位算错；现在统一改为“掩码归约 + 无符号比较”，对 32 位和 64 位
计划都成立。

模型用一个变量 `v` 保存当前可变标志，但它的含义由 `family` 决定
（FAM_ADD 时是 carry，FAM_SUB 时是 borrow），并由 `flags_defined` 标记是否
已定义。每次条件物化、条件跳转或 ADDC/SUBC 消费前都调用
`model_require_flags()`，通过 `TASK_CHECK` 保证**永远不会拿未定义的标志做
比较**。这与 `sljitLir.h` 的约定一致：消费者必须先有带 `SLJIT_SET_CARRY`
（宽度匹配）的已定义生产者。

## 2. 为什么续接点确实跨过 status_flags_state

在 ARM64 后端里，`compiler->status_flags_state` 记录最近一次标志是由 ADD 族
还是 SUB 族、以 32 位还是 64 位产生的。它**只**被 `get_cc()` 读取，而
`get_cc()` 服务于两类消费者：

- `sljit_emit_op_flags` 发射的条件物化（CSET/CSINC）；
- `sljit_emit_jump` 发射的条件跳转（B.CC）。

`SLJIT_CARRY` 在 ADD 之后映射到硬件 CS，在 SUB 之后映射到 CC；这个二义性
正是靠 `status_flags_state` 区分的。与之相对，ADDC/SUBC 直接编译成 ADC/SBC，
读取硬件 C 标志本身，不查询该字段。

`status_flags_state` 由 `sljit_serialize_compiler` 写进
`sljit_serialized_compiler`，再由 `sljit_deserialize_compiler` 恢复。
`task_support/flags.patch` 把恢复语句改成置 0，于是恢复后的编译器丢失了
“最近标志来自 ADD 还是 SUB”。要让这个缺陷可观察，续接点必须满足：
**标志生产者在被序列化的一侧，而读取该状态的条件消费者在恢复之后的一侧。**

本测试的四个热窗口（检查点 N 表示发射完 `[0,N)` 后序列化，后缀从 N 开始）：

| 检查点（步骤边界） | 已发射侧的最后状态 | 恢复后才发射的消费者 |
|---|---|---|
| STEP_MOV_PRESERVE (4) | PROD1 已置 carry/borrow | ADDC/SUBC、MAT1 |
| STEP_MAT1 (7) | CONS1 已延续标志 | 条件物化 MAT1（op_flags） |
| STEP_JCC (12) | PROD2 已置 carry/borrow | 条件跳转 JCC |
| STEP_MAT2 (13) | 分支汇合、标志重新声明 | 条件物化 MAT2（op_flags） |

这些位置都不是“几次互不相干的加法”：计划里 PROD1 的结果进入至少三段的
ADDC/SUBC 进位/借位链（PROD1→CONS1→CONS2，第二段家族再由 CONS3 收尾），
MAT1 物化出的 0/1 又被后续普通 ADD 累加（STEP_USE_MAT），JCC 与 MAT2 消费
PROD2 的同一标志。因此每个热窗口都真实地跨越了“标志已产生、消费者未发射”
的编译状态。

突变版的确定性现象出现在 JCC 窗口：恢复后 `get_cc()` 读到被清零的
`status_flags_state`，把本应按 SUB 借位选择的条件码选错，分支走向相反，
写出的分支槽从期望值翻成另一个常量（日志中表现为 `slot=1 got=0xa7
want=0x51`），这是**真实输出差异**，不是崩溃、也不是机器码字节比较。原有的
138 项测试不经过“恢复后再发射条件消费者”的路径，所以突变版仍能通过它们。

### 关于标签处的标志重新声明

`sljit_emit_label()` 出于控制流汇合的保守考虑会把参数检查用的 `last_flags`
清零。本测试的分支两条路径只执行跳转和保持标志的 MOV/写存，硬件标志确实
仍然存活，因此在汇合标签后调用公开接口 `sljit_set_current_flags()` 重新声明
`SET_CARRY | SET_Z` 及 ADD/SUB、32/64 属性，MAT2 才是合法消费者。

## 3. 三条编译路线与生命周期

同一份计划走三条路线，且只比较外部可观察结果（输出槽、分支结果、进位链、
canary），从不比较两份机器码：

1. **直接编译**：单个编译器一次发射完，作为与模型对账的基线（contract_model）。
2. **单次续接**：在热窗口保存 → 释放编译器 → 反序列化恢复 → 用公开 emit
   接口发射后缀；序列化缓冲在继续使用前就释放，验证新编译器不依赖该缓冲。
3. **反复续接**：四个热窗口各自做一次保存/恢复，并额外在一个编译器生命周期内
   串联两个检查点（contract_resume）。

生命周期（contract_lifetime）额外覆盖：

- 同一份序列化缓冲分别反序列化成两个编译器，追加**不同的合法尾部**（一个
  原样收尾，一个额外物化反向条件到独立槽），各自生成并执行；先释放 A 的
  成品代码，确认 B 的代码仍可执行，编译器与成品代码的释放互不影响。
- 旧的 label/jump 指针不跨编译器复用：恢复后只用
  `sljit_get_first_jump`/`sljit_get_next_jump`/`sljit_get_label_index` 等公开
  索引重新取得，再 `sljit_set_label` 重新链接，并断言标签创建序号在序列化
  前后保持稳定。

## 4. 覆盖矩阵与运行方式

- 固定种子、确定性 PRNG，共 192 个不同计划（`PLAN_COUNT`），32 位与机器字宽
  各半；每个计划打印一行可单独复跑的 `PLAN id=...` 编码。
- 运行结束按类别打印**非零**计数（`STAT ...`）：ADD/SUB 生产者、ADDC/SUBC
  链操作、条件物化与条件跳转两类消费、进入链时 carry/borrow 为 0 和为 1 的
  数量、寄存器/可表示立即数/内存三种操作数、各续接位置次数、恢复次数、
  缓冲提前释放、双重反序列化、标签重取等。
- 内存只访问 C 侧 `struct ckpt_slots` 中显式分配的槽，输入区与输出区两侧
  各带 canary；每次运行都核对 canary、输入区未被改写、全部输出槽与模型一致，
  32 位结果只按对应位宽比较。出现差异时打印计划编码、续接位置、初始数据和
  第一个不同的槽。
- 复跑单个计划：设置环境变量 `SLJIT_CKPT_ONLY=<id>`。
- 分配失败、无法生成代码或环境不支持一律打印 `SETUP_FAILURE` 并返回负值，
  由包装侧计为运行失败，不会伪装成行为偏差；内容断言失败固定以退出码 41
  结束并打印 `CONTENT_DIFFERENCE <group>`。
