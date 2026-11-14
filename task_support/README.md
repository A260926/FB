Requires only a C++17 compiler, bash and standard system utilities. No Qt, CMake, Python or GPU is used by these build entries.
Run bash task_support/run.sh baseline or bash task_support/run.sh target.
New tests include "task_support/check.h" and define task_cases and task_case_count.
Each task_case is {group,name,function}; function returns int and uses TASK_CHECK(expr).
Register face, interior, seams, orientation and invalid. Do not define main.
The supplied driver rejects empty groups and functions with no executed checks.
production_smoke.cpp compiles and calls the new public extractor on an ordinary plane before the new regression groups. This is a normal-case build entry, not a topology implementation.
The baseline invokes the original extractor, counts the actual connected components and prints the analytic comparison without claiming the new behavior exists.
