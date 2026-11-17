公开接口扩展写在 include/MemPDB/MemPDB.hpp，原字段保持原顺序，追加字段即可：
```cpp
enum class ArgumentLocationKind : uint8_t { Register, RegisterRelative, FramePointerRelative };
struct ArgumentLocation {
    uint32_t BeginRVA, EndRVA;
    ArgumentLocationKind Kind;
    uint16_t Register;
    int32_t Offset;
};
// Argument 追加 std::span<const ArgumentLocation> Locations;
```
Register 表示寄存器中的值（Offset=0）；RegisterRelative 表示寄存器+有符号Offset处的存储位置；FramePointerRelative保留抽象帧基址+Offset（Register=0）。不读取寄存器或目标内存。BeginRVA/EndRVA为半开区间。

S_LOCAL(0x113e)的payload为u32 type、u16 flags、NUL名称，flags bit0=parameter、bit8=optimized-out。只收当前过程直接作用域的parameter，按声明次序关联已有TPI形参槽；优化掉的参数仍占自己的槽，Locations为空。不扩展隐式this、可变参数、内联或piece。

支持S_DEFRANGE_REGISTER(0x1141)、S_DEFRANGE_FRAMEPOINTER_REL(0x1142)、S_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE(0x1144)、S_DEFRANGE_REGISTER_REL(0x1145)；不支持subfield/spilled member。前三种带range的形式使用CodeView原结构：register形式u16 reg/u16 attr，frame-relative形式i32 offset，register-relative形式u16 reg/u16 flags/i32 offset，随后均为u32 sectionOffset/u16 section/u16 length，余下为u16 gapOffset/u16 gapLength。FULL_SCOPE只有i32 offset。普通register-relative要求spilledUDTMember和offsetParent为0。

range从所属映像节表转换RVA，gap相对于该range起点；先移除gap并裁到过程范围，保留非空片段。FULL_SCOPE覆盖过程完整区间。结果按原DEFRANGE次序展开，每条内部按地址升序，不合并不同记录也不补洞。相邻范围右端不包含。只有紧随当前S_LOCAL的range归它，非参数local也截断旧归属；S_BLOCK32及其S_END正确配对，不得提前终止外部过程。本文输入使用完整有效CodeView记录，不要求扩大为通用损坏映像检查。

现代S_LOCAL参数已出现的过程不再把REGREL/FRAMEREL局部变量按位置凑为参数；没有现代参数的旧过程保留原路径及Offset。现代参数Offset保留0，位置由Locations表达；已有Size字段行为保留。找不到名字/位置的TPI槽仍保留，不能缩短ArgumentCount。所有现代记录来自同一个过程；同名不同地址的public通过名字和节解析RVA匹配自己的proc，不能只取名字lower_bound第一条。旧名字查找仍返回name-sorted表中第一项；FunctionAt可遍历每个同名public。

Locations的数组属于PDB结果，不借用临时模块vector、解析栈或string arena；PDB move后旧Function/Argument视图在目标PDB存活期间继续可读，源对象可以销毁。InternStrings开关、碎片流、ResolveArguments=false以及已有旧函数/全局/结构接口保持兼容。

格式参考Microsoft公开CodeView定义：https://github.com/microsoft/microsoft-pdb/blob/master/include/cvinfo.h ，只作为格式说明，运行不联网、不依赖该仓库。
