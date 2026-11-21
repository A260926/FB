基线：bash task_support/run.sh baseline。程序实际构建稀疏体素DAG，bake后运行ESVO和参考射线，与独立体素盒相交核对命中/距离/材质。
目标：bash task_support/run.sh target。新增测试放 tests/dag_raster_target.cpp，包含 task_support/check.h，不定义main；定义全局 const struct task_case task_cases[] 和 const size_t task_case_count，组名为 coverage、hierarchy、sparse。每个函数用 TASK_CHECK，成功返回0，每组至少一次实际内容检查。
新增 dag_rasterizer.h/.cpp、depth_hierarchy.h/.cpp 与 examples/dag_preview.cpp 都是必需输入。入口强制编译生产源码及头文件并链接，先删除旧PPM再运行示例，随后执行新测试和原生产基线。
使用上游自带 poolstl.hpp 的标准库实现分支，不启动SDL/OpenGL应用，也不从系统包缓存或网络解析第三方依赖。
未附带待实现的目标功能、新测试或目标示例。
