运行 bash task_support/run.sh baseline 检查原有生产路径。完成后运行 bash task_support/run.sh target。
新 tests/LodSurfaceTarget.cpp 必须包含 task_support/check.h，以 task_cases 和 task_case_count 注册 seams、edits、reuse。每例返回0并使用 TASK_CHECK；不定义main。每组至少一例且真实执行断言。
例：static int case_fn(){ TASK_CHECK(condition); return 0; }
const struct task_case task_cases[]={{"seams","case",case_fn}, ...};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
LodSurface.h/.inl 和 examples/LodSurfaceHeadless.cpp 均须非空；头文件强制进入测试与示例编译。示例缺失、编译或运行失败均失败。运行器最后联跑原有正常功能。
