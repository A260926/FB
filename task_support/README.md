生产源码为 pbrt-v1，COPYING.txt 中为 GPL-2.0-or-later。原来的动态插件以工厂符号重命名后静态链接；所有相关几何、加速器、材质、灯和输运函数保留。task_support/native.h 声明重命名后的工厂，TaskEmptyVolume 仅为无体积场景返回单位透射，不替代任何表面实现。原 EXR/解析器/动态插件程序仍在树内，本入口直接构造 ParamSet 和 Scene，不需要 EXR 或 flex。build.sh 使用 -Wno-narrowing 保留旧版 RandomUInt 到 u_int 的隐式转换；生产文件未修改。

基线：bash task_support/run.sh baseline。未来新增测试与示例：bash task_support/run.sh target。测试必须包含 task_support/check.h，注册 task_cases/task_case_count，以 TASK_CHECK 返回失败，不定义 main；geometry、density、transport 各至少一个执行了断言的用例。示例先编译并运行，motion.ppm 必须是本次新生成的非空文件。禁止将 skip 或进程提前退出当作成功。
