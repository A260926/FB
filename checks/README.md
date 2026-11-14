This seed contains input support only. It does not contain the requested model or target tests.
Run bash checks/baseline.sh for existing production behavior; bash task_support/run.sh target compiles and runs the new target file.
The target file registers task_cases/task_case_count and uses TASK_CHECK. Each failed content comparison must print CONTENT_MISMATCH with its location, expected and actual values before returning through TASK_CHECK. Setup failures must be reported separately.
Run bash checks/run-mutants.sh only after the normal target passes. It recompiles three isolated source copies and accepts only driver status 4 with both CONTENT_MISMATCH and FAIL records. Build failure, signal termination, and mere nonzero exit are rejected.
All upstream licenses and tests are retained. Mutation patches are test inputs, not production changes or target answers.
