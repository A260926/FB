Use bash task_support/run.sh baseline for the existing MMGS path.
The target mode requires tests/periodic_target.c, src/mmgs/periodic_s.c and libexamples/mmgs/periodic_headless.c.
The C target uses check.h and task_cases/task_case_count without main; every declared group must execute actual checks.
The example receives one output mesh path. Generated output is removed before invocation.
No periodic implementation or target test is supplied.
