This directory contains only the starting-point rendering smoke test and a fixed C callback runner.
It contains no portal implementation, no portal regression cases, and no portal example.
Run `sh checks/run.sh baseline` from the repository root. The smoke test calls the unmodified
upstream `test_draw_scene` using a tiny generated OBJ fixture and three actual material constructors.
The upstream window/demo entry points are not invoked; the linker removes their unused platform code.
`target` compiles tests/portal_contract.c with contract_main.c, checks callback completion outside
the process, then separately compiles examples/portal_preview.c. Output is in .portal-build/.
All new support code and triangle.obj are supplied under the repository MIT license.
