MLT C framework source subset. See SOURCE.json, COPYING and source headers.
The original CPU luma/composite and timewarp sources are retained as context;
this build compiles the framework only, with no external modules or devices.

Run the original production witness with bash task_support/run.sh baseline.
After adding the conform producer, test and example, run
bash task_support/run.sh target. It rebuilds the library, runs the fixed
public API consumer, creates and checks a fresh WAV, and runs clock/lazy/graph
regression groups. The fixtures generate input only; no conform implementation
is supplied. No Python, network, plugin download or package cache is needed.
