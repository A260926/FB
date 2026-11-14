Baseline: bash task_support/run.sh baseline
Target: bash task_support/run.sh target
Requires CMake >=3.30, a C++17 compiler and a local build tool. All optional importers, tools, samples and external tests are disabled.
The target wrapper explicitly compiles the new production .cc into the offline library. The new .cc must also be added to src/animation/offline/CMakeLists.txt; its header must be in the public HEADERS file set.
Tests include "task_support/check.h", return int (0 means success), and use TASK_CHECK(expr).
Define the exported task_cases array and task_case_count using struct task_case entries {group,name,function}.
Required groups: reduction, shear, mapping, runtime, invalid. Each must execute at least one function containing a numerical or behavioral assertion. Do not define main; driver.cpp supplies it.
The baseline exercises existing SkeletonBuilder, SampleAnimation and SkinningJob and intentionally demonstrates a non-TRS composition; it does not implement reduction.
