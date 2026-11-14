EH3 x64 范围说明

只解释静态首次 C++ throw 的候选控制流，不替代 Windows runtime 的类型匹配及异常重入处理。输入 handler RVA 是调用者确认的语言处理器身份；不识别跳板，不执行 handler。不支持 CHAININFO、EH4、动态状态修正及 EHFlags 中 0/1 以外的位。

Exception Directory 每项是三个 little-endian uint32：BeginAddress、EndAddress、UnwindInfoAddress。范围为 [Begin,End)。UNWIND_INFO 前四字节依次为 version/flags、prolog size、CountOfCodes、frame register/offset；每个槽2字节，槽数向偶数补齐后，语言handler RVA在4+2*round_up_even(count)处，随后一个uint32为FuncInfo RVA。只接受EHANDLER/UHANDLER的非链式关联。需要核对函数范围不重叠、代码和action/catch RVA可映射到可执行节。未知personality不尝试解释它的数据。输入PE只能读取，不能修改其全局错误语义以绕过旧行为。

FuncInfo 固定40字节，按4字节顺序：magic、MaxState、UnwindMapRVA、NumTryBlocks、TryBlockMapRVA、IPMapEntries、IPToStateMapRVA、UnwindHelp（有符号帧偏移）、ESTypeListRVA、EHFlags。MaxState是状态总数，合法编号0..MaxState-1；仅支持ESTypeListRVA=0。数量乘长度和RVA加法在读取前核对。零数量允许零RVA。所有元数据以真实RVA逐字节经生产读取设施读取，不能假设一张表只在一节。

UnwindMap每项8字节：signed ToState、action RVA。目标范围为[-1,MaxState)。表按数组索引命名状态，不能用action地址当节点身份。所有状态必须最终到达-1。查询计划在到达停止状态前记录每次状态转移（包括action=0），不是状态序号排序；对于候选catch，指定停止状态必须实际出现在链上。

TryBlockMap每项20字节：signed TryLow、TryHigh、CatchHigh，NumCatches、HandlerArrayRVA。要求0<=TryLow<=TryHigh<CatchHigh<MaxState。保护区间[TryLow,TryHigh]必须层叠或不相交，完全相等时以原表序号稳定处理。内层以保护区间包含关系决定，不能依赖表写入顺序。每个HandlerType为20字节：Adjectives、TypeDescriptorRVA、signed CatchObjOffset、HandlerRVA、signed ParentFrameOffset。不解引用类型描述符，不推测动态类型，原样记录这些字段和原表次序。

IPToStateMap每项8字节：IP RVA、signed state。IP严格递增，state必须在[-1,MaxState)。每个已关联runtime range分别按全局映射的前驱项确定状态，再按实际边界裁剪；不可把不同runtime range之间的空洞包含进来。state属于任一(TryHigh,CatchHigh]时查询返回不支持；此外调用者通过查询参数声明是否为普通函数体首次throw，false返回不支持，避免假装能从静态表识别所有cleanup执行情形。

图保留FuncInfo、runtime项、各映射/状态/try/catch的RVA来源。成功结果独立拥有，不借用parsed_pe；失败清空输出并提供来源RVA与原因。按函数起始RVA和区间起点稳定排序，路径按内层try及catch原序，再附escape路径；escape路径只表示没有类型匹配时的静态展开，不声称一定发生。

格式核对来源（不作为构建输入）：https://github.com/llvm/llvm-project/blob/llvmorg-20.1.3/llvm/lib/CodeGen/AsmPrinter/WinException.cpp 的 emitCXXFrameHandler3Table；清理停止状态核对 Wine dlls/msvcrt/except.c 的 cxx_local_unwind / cxx_frame_handler。
