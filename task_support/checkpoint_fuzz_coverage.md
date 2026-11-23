# checkpoint_fuzz 结构化回归覆盖说明

本文件说明 `test_src/checkpoint_fuzz.c` 的独立整数模型如何区分 ADD 的进位
（carry）与 SUB 的借位（borrow），以及为什么选定的序列化续接点确实跨过了
`status_flags_state` 这类只存在于编译器内部的标志状态。

## 1. 计划与三种执行路线

每个计划（plan）是一段确定性的符号程序，固定种子可单独复跑：

1. 一个根 `SLJIT_ADD`/`SLJIT_SUB`（带 `SLJIT_SET_CARRY`），后面接两个
   `SLJIT_ADDC`/`SLJIT_SUBC`，因此进位/借位链至少三段，且前一段的结果寄存器
   是下一段的第一个源操作数，段与段之间不是互相独立的加法。
2. 每段算术后有一条不修改 flags 的 `SLJIT_MOV`/`SLJIT_MOV32`，再由
   `sljit_emit_op_flags` 把条件物化成 0/1 存入 C 侧槽位。
3. 计划末尾再用一种条件消费：要么把物化条件累加进后续计算，要么用条件跳转在
   两个不同常量之间二选一写出。

同一份计划走三条路线，并且三条路线都与同一个独立模型逐槽比较，而不是比较两份
机器码是否字节相等：

- 一次性编译（`contract_model`，也负责模型/直编一致性）；
- 在 flags 产生与首次消费之间做一次保存-释放-恢复（`contract_resume` 的单续接）；
- 同一份计划在多个合法位置反复保存恢复（`contract_resume` 的三续接，以及
  `contract_lifetime` 中“一次保存、恢复成两份、各自追加合法尾部”）。

## 2. 模型如何区分 ADD 进位与 SUB 借位

所有运算都在无符号类型 `sljit_uw` 上做，取模由 C 的无符号回绕定义保证，不产生
任何 C 有符号溢出，也不读取 ARM 条件码或 sljit 的状态分类函数。

位宽掩码 `mask` 在 32 位计划里是 `0xFFFFFFFF`，在机器字宽计划里是全 1；32 位
结果只按该掩码比较。

- ADD 家族
  - 根 `ADD`：`r = v + x (mod 2^N)`，`carry = (v > mask - x)`。
  - `ADDC`：`r = v + x + carry_in`，两次无符号比较得到的进位相或，即任意一层
    超出模数就产生进位。
- SUB 家族
  - 根 `SUB`：`r = v - x (mod 2^N)`，`borrow = (v < x)`。
  - `SUBC`：`r = v - x - borrow_in`。这里 `borrow_in` 就是上一段留下的借位
    （ARM64 `SBC` 的语义是 `Rd = Rn - Rm - C`，C 即上游借位），两层无符号
    比较 `v < x` 与中间结果 `< borrow_in` 相或得到新借位。

`SLJIT_CARRY` 在 ADD 后表示无符号溢出、在 SUB 后表示“第一操作数无符号小于第二
操作数”（即借位）；`SLJIT_NOT_CARRY` 取其逻辑反。模型只用上述无符号比较推导，
完全不依赖具体架构的 N/C 标志位命名。

## 3. 为什么续接点跨过了这类编译状态

在 ARM64 后端（`sljitNativeARM_64.c`）里，算术指令发射时会写入编译器内部字段
`compiler->status_flags_state`（`SLJIT_CURRENT_FLAGS_ADD` /
`SLJIT_CURRENT_FLAGS_SUB`，可再叠加 `SLJIT_CURRENT_FLAGS_32`）。真正选择机器
条件码的 `get_cc()` 依赖这个字段：

- `SLJIT_CARRY`：若 `status_flags_state & SLJIT_CURRENT_FLAGS_ADD` 选 ARM64
  的 CS（无符号进位），否则落到 CC（借位）；
- `SLJIT_NOT_CARRY`：ADD 情况下选 CC，否则落到 CS。

也就是说，ADD 与 SUB 复用同一个 sljit 条件枚举，靠编译器内部记住的“上一条是
加法还是减法”来翻译成相反的硬件条件码。`sljitSerialize.c` 会把
`status_flags_state` 写进序列化缓冲，并在反序列化时恢复。

本回归选择的续接点（`CP_PREMAT0/1/2`、`CP_POSTMAT0/1/2`、`CP_PREJUMP`）全部位于
“带 `SLJIT_SET_CARRY` 的算术已经发射、但第一个 flags 消费者还没发射”之间：

- `PREMAT*`：算术和保 flags 的 MOV 已发射，`op_flags` 物化尚未发射；
- `POSTMAT*`：本段条件已物化，但下一段算术/下一次消费尚未开始，flags 仍处于
  已定义状态；
- `PREJUMP`：最后一段 flags 存活，条件跳转尚未发射。

在这些位置保存并释放编译器后，新编译器只能靠缓冲里恢复的 `status_flags_state`
才能在后续 `get_cc()` 中选对硬件条件码。`task_support/flags.patch` 把反序列化中的
该字段强行清零后，直编路径（不经过恢复）不受影响，原 138 项测试也全部通过；但
恢复后编译的程序在 ADD/SUB 的 carry/borrow 消费点会选反条件码，物化出的 0/1 或
条件跳转写出的常量与独立模型不同，于是被真实的输出差异拒绝（退出码 41，
`CONTENT_DIFFERENCE resume`），而不是靠宏、文件路径或预知改动直接判失败。

## 4. 覆盖矩阵与内存安全

固定种子生成至少 160 个不同计划，运行时按类别打印非零计数，覆盖：

- 32 位与机器字宽（64 位机上即 64 位）两种边界；
- ADD/ADDC 进位与 SUB/SUBC 借位两个家族；
- 上游进位/借位取 0 与取 1 两种状态（含贴近溢出边界的输入）；
- `SLJIT_CARRY` 与 `SLJIT_NOT_CARRY` 两种消费；
- 条件物化（`op_flags`）与条件跳转两种消费形态；
- 第二操作数的寄存器、可表示立即数、内存三种形式；
- 续接位置计数与恢复次数（单续接、同计划三续接、双恢复各自尾部）。

所有内存操作数只访问 C 侧 `malloc` 风格数组中明确分配的槽位；该数组在工作槽前后
各放一个 canary，每次执行都校验两个 canary 未被破坏，并校验输出槽、分支结果、
整条 carry/borrow 链。分配失败、无法生成代码或环境不支持时只计为运行失败
（`runtime_failures` 类别），不会被当成行为偏差。

## 5. 生命周期与跨编译器指针

- 反序列化得到的新编译器，在 `SLJIT_FREE` 掉序列化缓冲之后继续发射并生成代码。
- 一次序列化的内容分别恢复成两个独立编译器，追加不同的合法标记尾部后各自生成、
  执行；先释放第一份成品代码再运行第二份，确认编译器与成品代码的释放互不影响。
- 跳转在保存前创建且不绑定标签；恢复后不沿用旧 `sljit_jump*`，而是通过
  `sljit_get_first_jump`/`sljit_get_next_jump` 枚举与 `sljit_get_label_index`
  在新编译器上重新取得跳转和标签再绑定，符合 sljitLir.h 的公开索引约定。
