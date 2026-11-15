bash task_support/run.sh baseline运行旧存储、压缩、射线和Glyph路径；完成后用 bash task_support/run.sh target。
测试包含 task_support/check.h，定义 task_cases 和 task_case_count，注册 correlation、clipping、sharing 三组。每个case在实际比较处调用TASK_CHECK，成功返回0；不定义main。空组、无检查、提前exit、缺少测试都不能通过。
必需生产头/实现和示例均强制非空；两个新增实现均进入真实链接。例子必须生成.task-build/beam.ppm；该入口先删除自身旧产物，再运行示例和测试。
