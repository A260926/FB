# 支持范围与坐标

这是 CodeView 内联行信息的有限离线读取接口，不是进程调试器。公共输入为 RawFile、/names 流号，以及 ModuleSlice 数组；每项给 moduleIndex、streamIndex、symbolBytes、c11Bytes、c13Bytes。symbolBytes 包含4字节模块签名；C13从 symbolBytes+c11Bytes 起，长度 c13Bytes。MSF页目录由调用方保证合法；模块边界及消费的内容由新接口检查。可以自行设计返回类型及错误类型。

处理 S_GPROC32/S_LPROC32（含 _ID）、S_BLOCK32、S_INLINESITE、S_INLINESITE_END、S_END，其他无作用域记录可跳过。parent/end 是从模块符号流起点计的记录偏移，不是数组序号。内联 parent 可以经 BLOCK32 回到过程或外层内联；end 必须落到相应终止记录，不能跨过程。代码坐标始终相对于最外物理过程的 section/offset；内层注解不能再次叠加父内联区间起点。暂不处理分离代码块、INLINESITE2、IPI名称恢复及OMAP。

C13处理 INLINEELINES 的签名0与1、FILECHECKSUMS；额外文件数组按记录内声明长度消费，不把其元素误当作下一inlinee。文件身份是本模块checksum子节内的记录字节偏移，再由该记录的filenameOffset读取全局/names字符串。相同inlinee编号和checksum偏移可在不同模块表示不同内容。文件路径只按存储值复制，不读磁盘。重复inlinee定义内容矛盾须报错。

# 注解约定

格式来源：[Microsoft cvinfo.h](https://github.com/microsoft/microsoft-pdb/blob/master/include/cvinfo.h) 的 BinaryAnnotationOpcode、CVUncompressData、DecodeSignedInt32。操作码和操作数均为1/2/4字节压缩无符号整数（最高前缀0/10/110，其余非法，最大0x1fffffff），有符号数低位为负号，其余位为幅度。不是ULEB128。0是终止padding，其后的字节必须全0。

状态初始为过程内代码偏移0、inlinee基准行号和文件、行跨度1、statement=true、无列。支持1、3–13号操作；2号只接受0，非0报告unsupported，不猜分离块位置。1号CodeOffset设置过程内偏移。3号按delta推进后产生行记录。4号ChangeCodeLength为最近未定长记录给出显式长度，并推进代码游标。5号换文件，6号加有符号行差，7号设置源行跨度，8号设置statement(1)/expression(0)，9号设置列起，10号给已有列末加有符号差，13号设置列末；列0表示未指定，没有列末时的10号不建立列末。11号一个操作数低4位为代码delta，高位按上述有符号格式表示行差，更新两者后产生记录；12号先读length再读codeDelta，推进后产生显式定长记录。状态变化只影响之后产生的记录。

上一条未定长记录由下一产生记录的起点封口；最后仍未定长的记录没有可证实右界，不扩展到过程末端、不参与地址命中。所有区间半开，空区间不命中。代码回退、范围越出32位坐标或物理过程、非法行列、截断操作数及无效checksum引用返回包含moduleIndex、streamIndex和逻辑字节偏移的错误。已知长度产生的洞保留，不能补到下一段。每个site保留原始注解起止偏移和记录身份；可相邻合并的显示结果也不能丢原始证据。

按节号和节内PC查询，返回所有可覆盖该PC的最大父子链；每条链包含物理过程身份及外到内的内联帧。内联帧有moduleIndex、symbol record偏移、inlinee ID、区间、文件、行跨度、可选列和statement，不能仅按inlinee或路径去重。子记录的有效区间与全部祖先内联覆盖相交；父级空洞中不能单独出现子帧。兄弟区间重叠时各成一条链，排序按moduleIndex、物理过程记录偏移、链中site记录偏移字典序稳定。没有内联覆盖返回空集合。构建后拥有全部数据；查询不重放注解，不每次扫描全部site。
