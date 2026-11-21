运行 bash task_support/run.sh baseline 查看原有 RawVolume/PagedVolume 的跨零坐标立方体提面。
运行 bash task_support/run.sh target 构建新生产头文件、新示例及 tests/light_target.cpp，然后执行三组测试和正常生产基线。
新增测试包含 task_support/check.h，不定义 main；定义全局的 const struct task_case task_cases[] 与 const size_t task_case_count。每组至少一个函数，每个函数至少一次 TASK_CHECK，返回0表示通过。组名为 propagation、editing、lit_mesh。
新示例每次生成 .task-build/lit.ply；入口先删除旧产物，缺文件、编译失败、运行失败、空测试和零断言都应失败。
这里没有预置目标功能、新测试或示例。
