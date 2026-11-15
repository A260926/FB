已有路径：bash task_support/run.sh baseline；完成后：bash task_support/run.sh target。
新tests/SurfaceLightsTarget.cpp 包含 task_support/check.h，注册exposure、distribution、transport三组，以task_cases/task_case_count提供实际用例，不定义main。每例返回0并用TASK_CHECK断言实际结果。
static int probe(){TASK_CHECK(condition);return 0;}
const struct task_case task_cases[]={{"exposure","case",probe}, ...};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
目标必须非空，不能零测试、全skip或提前exit(0)。未来src/VoxelSurfaceLights.hpp/.cpp、src/VoxelDirect.cpp与examples/SurfaceLightsHeadless.cpp必须非空；所有非SDL生产.cpp实际编译链接，示例必须编译执行成功。检查不下载素材、不启动SDL。
