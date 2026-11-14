The fixed upstream source and this added support are dedicated to the public domain; see the unchanged root README for the upstream author's declaration.

`bash task_support/run.sh baseline` compiles and runs the unchanged example.cpp and real trimesh.cpp, then checks disk adjacency, closed-shell adjacency, and clear/build. The baseline records existing topology operations and contains no solid subtraction implementation.

`bash task_support/run.sh target` compiles solid_difference.cpp with tests/solid_difference_target.cpp and the fixed driver. Include check.h, export task_cases and task_case_count, and use TASK_CHECK; do not define main. The required groups are intersections, solids, provenance, and validation. Each group must execute at least one case with at least one check. The shell preserves the real exit code and independently requires every group completion and final completion before it runs the old baseline. Keep the fixed support files unchanged.

The new production module, example, and full regression cases are intentionally absent. Only a C++11 compiler, Bash, standard C++ library, and grep are required; there are no packages, services, or runtime downloads.
