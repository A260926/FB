# Ownership replay contract
新增公开头 fuzz/ownership_replay.h 包含 stddef.h/stdint.h，声明：
```
struct qjs_ownership_report { size_t steps, created, finalized, comparisons, exceptions; };
int qjs_ownership_run(const uint8_t *data, size_t size, struct qjs_ownership_report *out);
```
成功返回0；整条无效计划返回1；模型/引擎偏差返回2并打印首差。out为本次清零后累加的真实观测。运行器不能终止进程。每次新建runtime/context，自行完成清理。steps是执行记录数，created/finalized是本次逻辑对象/真实逐身份终结数，comparisons是执行过的对象责任检查数，exceptions是进入pending exception的次数（包含正常拒绝产生的异常）。

计划没有头，0到1024条三字节记录(op,a,b)。根槽0..15，最多64个递增身份；固定holder由执行器创建，只有一个held属性。除别名外不创建对象图，不允许槽覆盖。未使用的参数必须0。预检只用逻辑状态，不调用引擎，必须检查整条合法性。

| op | 动作 |
|---|---|
|0|NEW a：空根a得到带终结身份的普通自定义类对象|
|1|DUP a,b：复制活根a到空根b|
|2|DROP a：释放根a并清空|
|3|THROW_TAKE a：JS_Throw消费根a并清空，替换旧pending|
|4|CATCH a：有pending时JS_GetException转交到空根a|
|5|GC：JS_RunGC并核逐身份终结集合|
|6|STORE_TAKE a：JS_SetPropertyStr(holder,"held",根a)，清空根a并替换旧holder责任|
|7|CLEAR：holder.held设为undefined，释放旧责任|
|8|REJECT_TAKE a：对预先设为只读的独立holder属性调用JS_SetPropertyStr，正常拒绝仍消费根a并清空；pending保存正常TypeError，按未计入对象身份的异常处理|
|9|CALL_THROW a：JS_Call调用原生回调，以根a作借用参数；回调JS_DupValue后JS_Throw，根a保持，替换pending|

REJECT产生的非追踪异常也可CATCH到根；此类根可DROP、DUP、THROW_TAKE，不能参与STORE/REJECT/CALL的对象动作，不能作为新追踪身份。身份集合和异常类型标签分开记。每步查已出现终结事件不能属于仍有责任的身份；GC时以及自动结束清理后查应释放集合，终结顺序不作约束。CATCH只在有pending时合法。NEW不复用逻辑身份；活引用全释放后不得使用其旧根。空计划成功且所有计数为0。

ownership_model.c仅依赖自己的逻辑类型，不能包含quickjs.h；ownership_replay.c调用引擎API，ownership_fuzz.c导出LLVMFuzzerTestOneInput，受控拒绝无效计划，发现真实偏差才报失败。固定例和有效计划生成器可以在ownership_tests.c内。两次新runtime重放比逐步责任及终结集合（忽略终结顺序），不只比较最终计数。
