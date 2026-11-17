用 bash task_support/run.sh baseline 跑上游导入/动画回归及单树生产基线；target 模式会重编译 src/*.c、导入库，再链接新增 tests/forest_target.c。
新增测试包含 task_support/check.h，定义 const struct task_case task_cases[] 和 const size_t task_case_count，不定义 main。
按 import、hierarchy、batch 注册非空用例，在每个用例内对真实输出使用 TASK_CHECK，正常 return 0。三组都须有真实检查。
运行库的向量后端使用 x86 指令；Linux x86_64 直接编译，Apple Silicon 用系统 clang 的 -arch x86_64 及已安装的 Rosetta 运行。没有可用向量后端时测试标量并明确报告不可用后端。
