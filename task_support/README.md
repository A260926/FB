bash task_support/run.sh baseline 编译src/library/*.cpp并调用已有combine、voxel和真实intersectVolume。target模式还链接新增tests/combine_translated_target.cpp（以及未来新增的src/library/*.cpp）。
测试包含task_support/check.h，不定义main，以const struct task_case task_cases[]和const size_t task_case_count注册placement、aliasing、sparse三组，在每个用例实际断言处调用TASK_CHECK，正常返回0。
使用C++20、标准线程和仓内原样PoolSTL头，避免上游应用CMake的TBB/SDL依赖；不安装下载任何包。
