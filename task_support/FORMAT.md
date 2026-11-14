# compact unwind 只读范围

支持little-endian Mach-O64、CPU AMD64的普通独立映像；preferred VM地址来自GetBaseAddress，不接受运行时slide，不对查询PC做返回地址减一。无__TEXT,__unwind_info返回空索引。输入只读，不读进程栈，不执行handler。编码格式来源：[LLVM libunwind compact_unwind_encoding.h](https://github.com/llvm/llvm-project/blob/main/libunwind/include/mach-o/compact_unwind_encoding.h)，逻辑函数与范围及间接personality关系可查同项目UnwindCursor.hpp。以下说明足够离线使用，不需要下载这些链接。

# 两级索引与来源

版本1头为7个u32：version、common数组节内偏移及数量、personality数组节内偏移及数量、首级index节内偏移及数量。首级每项3个u32：functionOffset、secondLevelPagesSectionOffset、lsdaIndexArraySectionOffset。最后一项是哨兵，页偏移为0，只给末端函数偏移与LSDA表尾，不产生范围；空表可以只有哨兵。有实际页时functionOffset严格递增。每页最后条目到下一首级项的functionOffset截止，不使用文件中下一个物理页的位置。

二级页kind=2：u32 kind、u16 entryPageOffset、u16 entryCount，entry为(functionOffset,encoding)两个u32。kind=3：前述头之后还有u16 encodingsPageOffset、u16 encodingsCount；entry为一个u32，其低24位是相对此首级functionOffset的增量，高8位是编码索引。先索引全局common表，超过commonCount后再减去它访问本页local表。各PageOffset只相对于所属二级页，SectionOffset只相对于unwind_info节，functionOffset及LSDA目标偏移相对于image base。每页只消费所声明数组，不能借下一个页补字节；数组必须位于该页起始后的4096字节及节内范围。不同页可以反序存放。页内条目严格递增，首条须对应首级起点；不接受无条目的实际页。范围为相邻条目的半开区间，零编码也保留为no_unwind，不能被相邻范围吞并。

0x80000000（IS_NOT_FUNCTION_START）表示同一逻辑函数的后续范围；需回溯同页最近未置位条目。页首不能带此位，不猜跨页父函数。保留RangeStart和FunctionStart两种身份，切勿把续段起点当作逻辑函数起点。0x40000000表示HAS_LSDA，0x30000000是personality的一基索引，0表示无personality。每个首级LSDA子表以当前和下一首级的lsdaIndexArraySectionOffset为界，每项(functionOffset,lsdaOffset)各u32，按函数偏移排序；HAS_LSDA以逻辑FunctionStart精确匹配，不能用最近前驱。输出LSDA的preferred VM地址及原表项偏移，不解释其内容或猜长度。

personality数组每项为有符号32位image-base差，指向存储指针的间接槽。保留表项字节位置、slot VM、slot文件位置和原始8字节；这不是可以直接执行的函数地址。不应用dyld修补、不解认证指针、不跟到槽内容指向的对象。LSDA、槽地址和STACK_IND读取均使用File现有映射，须完整落在文件支持范围，不能从零填充或另一节补足。

# x86_64 符号化规则

输出表示已进入函数body后的元数据规则；不分析部分prolog/epilog，不求实际寄存器值。CFA表示调用者RSP，保存槽统一为有符号CFA相对偏移，并保留来源编码字。RBP_FRAME(mode=1)的CFA是RBP+16，RBP位于CFA-16，返回地址RIP位于CFA-8；offset字段在位16–23，五个3位槽在低15位，槽0表示空但仍占位置，从RBP-8*offset起按地址递增。槽内1–5依次为RBX、R12、R13、R14、R15，不允许重复或覆盖固定RBP/返回槽。

STACK_IMMD(mode=2)和STACK_IND(mode=3)有相同的保存寄存器排列格式：位10–12为寄存器数，低10位是从RBX、R12、R13、R14、R15、RBP中取有序不重复列表的排列编码，采用公开compact unwind排列规则，不能直接把bit段当寄存器号。IMMD的栈大小为位16–23乘8；IND的位16–23是相对逻辑FunctionStart的立即数位置，读出little-endian u32后，加上位13–15乘8。IND保留立即数VM、文件位置及4字节，不扫描或执行指令。栈大小包含返回地址，CFA为RSP+stackSize；返回地址是CFA-8，n个寄存器依排列顺序放在CFA-8-8*n开始的连续8字节槽。非法数量/排列、过小或不对齐栈以及重复保存槽返回错误。

mode=4为DWARF fallback：低24位是__eh_frame节内FDE偏移，保留偏移和真实文件位置，但不生成compact规则、不解CFI。没有该节或偏移不可读须报错。encoding=0的no_unwind与未知mode的unsupported分别表达；未知mode保留原值不造规则。无新规则不影响其元数据范围和来源被查询。

# 错误与拥有关系

算术不得回绕；截断头/数组、非法编码索引、错误首级或页内顺序、无父续段、LSDA标志找不到精确记录、非法personality索引及上述文件读取失败，都返回包含__unwind_info节内偏移和可用时条目VM位置的错误，不发布半份索引。容器通用验证仍由已有NewFile负责。所有范围保留首级、二级页、条目、编码(common/local/inline)的物理身份及文件字节位置，相同编码的相邻范围不能因此失去来源。构建后可关闭File并释放原字节，Lookup只查询已建索引，不重读文件或每次扫描全部条目。
