运行 bash task_support/run.sh baseline；实现完成后运行 bash task_support/run.sh target。
新测试包含 task_support/check.h，用 task_cases/task_case_count 注册 projection、balance、history 三组；每个函数返回0表示成功，在实际比较处调用 TASK_CHECK。不要定义 main。例：static int case1(){TASK_CHECK(actual == expected);return 0;} 然后提供 const struct task_case task_cases[] 和 const size_t task_case_count。
例子必须在工作目录生成 .task-build/lod-near.obj、.task-build/lod-far.obj；target每次先移除这两个自生成文件，强制编译并运行例子后再运行测试。
