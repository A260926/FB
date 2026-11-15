The baseline builds the original DLOD and CLOD on generated geometry and executes both.
Target requires tests/skinned_target.cpp and example/skinned.cpp. Include check.h and define task_cases/task_case_count without main; use TASK_CHECK in every group.
The example takes one output directory and must produce lod0.obj and lod1.obj. The runner removes both before invocation.
The original production implementation is linked into both tests and example. No skinned implementation or target tests are supplied.
