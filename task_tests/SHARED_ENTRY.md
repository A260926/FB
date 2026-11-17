# 共享入口（shared entry）结构化修复说明

## 问题形状

`task_support/fixtures/SharedEntry.java` 这类由普通 javac 编出的方法里有一个带标签的
块，其字节码控制流是：

```
        if (外层条件) goto SHARED
        if (内层条件) { ...; goto AFTER; }   // 绕过循环的旁路
SHARED: 共享初始化（一条或多条赋值/调用/字段读写）
HDR:    if (循环条件不成立) goto AFTER       // 循环头
        ...循环体...
        goto HDR                             // 回边
AFTER:  循环后的语句
```

三条路径在 `SHARED` 附近汇合：

1. 外层条件直接跳到 `SHARED`（不进内层 if）；
2. 内层 if 的 else 侧顺序落到 `SHARED`；
3. 旁路 goto（源码里的 `break 标签`）跳过 `SHARED` 和整个循环，直达 `AFTER`。

语义约束是：

- 旁路不能执行共享初始化；
- 进入循环但零次迭代的路径仍要执行共享初始化一次；
- 回边只能回到循环头 `HDR`，不能再执行共享初始化；
- `AFTER` 的语句既不能丢，也不能被收进任何一个分支。

## 根因

`ConditionalRewriter.considerAsSimpleIf` 处理内层 if 时，把从内层 else 起点一直到
`AFTER` 的整段（含共享初始化和整个 while）识别成了一个 `SIMPLE_IF_ELSE` 块，分支末端
的旁路 goto 被标记为 `GOTO_OUT_OF_IF`。此后 `ClassifyGotos.classifyAnonymousBlockGotos`
只接受 `JumpType.GOTO`，这条旁路再也无法成为匿名块 break，最终在输出里留下
`** GOTO` 和 “Unable to fully structure code”，重新编译失败。

## 修复

改动只在
`src/org/benf/cfr/reader/bytecode/analysis/opgraph/op3rewriters/ConditionalRewriter.java`。

在 `considerAsSimpleIf` 提交候选 else 区域之前，新增 `detectSharedEntryBypass`
（受 `labelledblocks` 选项门控，默认开启）。它同时满足下面三个结构性条件才认定为共享
入口，缺一不可：

1. 候选 else 区域内确实包含一个循环块（按 `BlockType.isLoop()` 判定）；
2. 该 if 分支末端 leave-goto 的落点，正是区域内某个循环的**出口块**——即回边所指循环
   头（while/for）或回边源（do/while）上、不带循环块标记的那个后继；
3. 区域起点之后的某个语句，存在一条来自区域之前、目标就是该语句、且尚未分类的前向
   跳转边。

命中后只做一件事：把分支末端的 leave-goto 标成 `JumpType.BREAK_ANONYMOUS`，然后返回、
让 `identifyNonjumpingConditionals` 的固定点重试。重试时该 if 不再满足 simple-if/else，
归约成一个纯 if；既有的匿名块通路（`AnonymousBlocks.labelAnonymousBlocks` 插入
`AnonBreakTarget`，`Block.extractLabelledBlocks` 生成带标签块，
`UnstructuredAnonymousBreak.tryExplicitlyPlaceInBlock` 生成 `break 标签`）自然把
区域包成带标签块。检测不按类名、方法名、变量名或固定偏移识别，条件取反、分支交换、
多重赋值、循环内 continue、循环后复用局部变量等变体都走同一条图判据。

## 共享入口在结构化区域里归到哪儿

以 SharedEntry 最终的 op03 图为准（节点编号为分析期临时编号）：

- 共享初始化节点和循环头节点**不带任何 `SIMPLE_IF_*` 块标记**，即它们不再属于内层 if
  的 else 块，而是匿名（标签）块内、if 之外的顺序语句；
- 旁路 goto 是 `break anon`，目标是位于循环之后的 `AnonBreakTarget`；
- 循环体节点带 `WHILELOOP` 标记，回边指向循环头而不是共享初始化，因此共享初始化在回边
  上不会重复；
- 循环后的语句位于所有 if/匿名块之外，所有路径（旁路与循环正常结束）都能到达。

SSA 局部值（例如 `saved`/`var4`）的创建点、循环体内的自增点和循环后的再赋值点与上述块
归属一致，变量作用域分析（`LValueScopeDiscoverImpl`）随后在这个正确的结构上正常运行，
不需要任何输出层补丁，也没有用异常占位。

## 哪些正常路径没有改变

检测条件刻意收窄，避免影响已能正确结构化的代码：

- **普通 if / if-else / 三元**：没有“区域外来前向跳入内部节点”，第 3 条不成立，走原
  路径；
- **无循环的标签块 break**：第 1 条不成立，保持原输出；
- **break 落点在循环出口之外的 else-if 链**（例如 JDK `String`、`HashMap` 里嵌套
  else-if 中含循环、但 break 跳到更外层汇合点的情形）：第 2 条不成立，保持原输出；
- **普通 while / for / do-while、continue、break、switch、try/catch/finally**：不经过
  该检测，完全不变；
- 关闭 `--labelledblocks false` 时整个检测被跳过，行为与修复前完全一致。

验证上，对 10 个 JDK 类（含 `java.lang.String`、`HashMap`、
`ConcurrentHashMap`、`LocalDate` 等）做了修复前后反编译输出的逐字节对比：全部一致；
本仓库自带的普通循环对照 `Ordinary` 以及所有既有探针输出也保持不变。

## 副作用与控制流保真

回归不仅检查文本。`task_tests/shared_entry_cases.cpp` 用 `javap -c -p` 把原始 class 和
重新编译出的 class 各自解析成控制流图，断言：

- 旁路 goto、循环头、循环守卫、回边、出口之间的前驱/后继关系在两个 class 上等价；
- 回边统一指向同一循环头，旁路与守卫汇合到同一出口；
- 副作用（方法调用、字段读写、赋值）按“旁路臂 / 共享初始化 / 循环体 / 循环后”四个区域
  归类，原始 class 与重编译 class 的区域序列完全相同；
- 零次迭代路径仍从共享初始化到达循环头，回边路径与旁路路径都不经过共享初始化。

素材方法只做编译和静态分析，从不执行。

## 运行

```bash
bash task_support/run.sh target
```

会编译生产源码与 `task_tests` 下的 Java 素材，运行 structure / effects / compatibility
三组回归，再执行基础样本（普通类成功、共享入口结构化且可重编译、Main 与 ApiDump 两个
入口都无结构化失败标记）。
