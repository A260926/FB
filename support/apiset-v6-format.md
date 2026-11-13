离线输入范围：API-set namespace version 6，所有整数uint32小端。偏移从namespace起点算，长度字段按字节，字符串UTF-16LE不含终止NUL。本任务先支持名称中的ASCII字符（UTF16高字节为0）；其他字符返回unsupported。

namespace头28字节，依次Version、Size、Flags、Count、EntryOffset、HashOffset、HashFactor。
entry每项24字节，依次Flags、NameOffset、NameLength、HashedLength、ValueOffset、ValueCount。
hash每项8字节，依次Hash、Index；共Count项，按Hash不降序排列，Index指entry。
value每项20字节，依次Flags、NameOffset、NameLength、ValueOffset、ValueLength。

HashedLength必须为偶数、不超过NameLength。条目名称的前HashedLength字节按ASCII小写的UTF16码元，以uint32环绕累积 hash=hash*HashFactor+codeunit。查询contract去掉可选.dll；其hash前缀截止最后一个'-'之前。按hash、前缀长度及不区分ASCII大小写的前缀找候选；同hash不同前缀不是同一contract。多个候选具有同一前缀时返回ambiguous，不任意选一个。报告保留完整entry名称。

value[0]是默认项（NameLength应为0）；之后为针对导入者basename的别名。具体别名匹配优先，匹配到空host表示禁用，不回落默认。没有别名时取默认；没有有效host则报告missing_host。比较basename不区分ASCII大小写并统一可选.dll后缀。重复规范化alias报错。

原始格式和语义参考（仅文档来源；运行不请求网页，也不复制Wine实现）：
https://www.winehq.org/pipermail/wine-cvs/2022-February/162567.html
https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/loader.c （get_apiset_entry、get_apiset_target）
