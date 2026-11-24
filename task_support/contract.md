# Scope replay contract
新增头 tests/fuzz/scope_replay.h 包含stddef.h/stdint.h并声明：
```
struct jerry_scope_report { size_t steps, created, finalized, comparisons, opened, escapes, takes, copies; };
int jerry_scope_run(const uint8_t *data, size_t size, struct jerry_scope_report *out);
```
返回0成功，1整条计划无效，2模型/引擎偏差；清零out后记录真实本次计数，函数不得退出进程。每次新jerry_init/cleanup周期（包括空计划），结束确认当前scope回到root且没有root注册值。steps是记录数，created/finalized按逻辑对象及真实终结身份计数，comparisons是已执行的责任检查数，opened/escapes是实际成功打开/逃逸数，takes/copies是WRAP/EXTRACT所有权分支次数，不包含COPY。

计划是0..4096条四字节(op,a,b,flags)记录，无头。ticket槽0..255；NEW递增对象身份最多128个；scope深度最多64。未用字段为0。所有使用到的ticket都须仍有活scope所有权，目的ticket必须从未用过或已随旧scope关闭失效。预检独立解释整个计划，不调用引擎，拒绝截断、无效槽及不平衡scope；不得把失效ticket传给引擎。不使用jerryx_remove_handle，不手动free已登记scope的值。

| op | 动作 |
|---|---|
|0|OPEN：新建escapable scope，当前scope压栈|
|1|NEW a：在当前非root scope登记新对象ticket a，附只记身份的native终结回调|
|2|COPY a,b：jerry_value_copy(a)后在当前scope登记ticket b；保留身份和普通/exception/abort标签|
|3|WRAP a,b,flags：flags 0/1分别为exception copy/take，2/3为abort copy/take；来源a仍被它的scope拥有。take先jerry_value_copy(a)生成独立临时责任再传给jerry_throw_value/abort(...,true)，copy直接传a和false。返回包装在当前scope登记为b；可将exception转abort或反向转换，身份不变|
|4|EXTRACT a,b,flags：来源须为exception/abort；flags 0调用jerry_exception_value(a,false)，1先copy(a)再jerry_exception_value(copy,true)。返回普通对象在当前scope登记为b，来源ticket责任不动|
|5|ESCAPE a：a须属于当前非最外层scope。调用jerryx_escape_handle；第一次成功把此ticket归属提升一层。当前scope已escape过时，另一个仍属此scope的a允许得到escape_called_twice且模型不变；不得传失效ticket|
|6|CLOSE：关闭当前scope并弹栈，使仍属该层的ticket失效；按公开契约逆序关闭|
|7|GC：jerry_heap_gc(HIGH)，逐对象核存活/终结集合|

每步检查已终结身份不再有活ticket；GC时要求零责任的所有对象已终结且各一次，终结顺序不作约束。最终所有显式scope已关闭，执行GC/cleanup再次核全部身份只终结一次。模型仅含逻辑ticket、身份、标签、scope归属，不能包含jerryscript.h、读取handle_prelist/handle_ptr或持有jerry_value_t；阈值路径只能由计划创造的scope/handle数量证明，不重做上游分配器。两次重放比较逐步责任和终结集合，而非比较原始引擎地址；两次独立初始化不能遗留root责任。scope_fuzz.c导出LLVMFuzzerTestOneInput，共用重放链，拒绝无效计划，只有真实偏差才报失败。
