Headless CAD build
==================

Run `bash task_support/run.sh baseline` from the source root. Run
`bash task_support/run.sh target` after adding `task_tests/target.cpp`.
The target uses `check.h`, `task_cases` and `task_case_count`; callbacks return
zero after their `TASK_CHECK` assertions. Required groups are `geometry`,
`propagation`, and `cache_export`.

The build compiles the original rational curve, surface, trim, polygon, shell,
mesh, Group and export implementations. Newly added C++ production files under
`src/` are compiled as well. The other shell caller in `solvespace.cpp` receives
a syntax check. No sketch solver, font discovery, GUI window, OpenGL, or web
resources are initialized. `host.cpp` supplies allocation, ordinary file paths,
a text dialog sink and the original tolerance-setting selectors. Its
`GenerateAll` is a host no-op: callers construct an already-generated
`runningShell` and group list in memory before calling `ExportMeshTo`.
`UpdateCenterOfMass`, `ShowNakedEdges`, and window invalidation are host no-ops.
Actual CAD triangulation, Group cache behavior, STL/OBJ writing and
`ExportMeshTo` come from the production source files. Center-of-mass UI,
font loading, whole-sketch regeneration and JS/HTML output are outside this
headless build.

`task_support` is the driver and environment boundary. Keep production changes
under `src/`; it is not a replacement for the geometry or export code.
