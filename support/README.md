This support code uses the upstream LGPL-2.1-or-later license (COPYING).

Run `bash support/run.sh build`, `baseline`, `legacy`, or `target`.
The target mode compiles `test/transparent_packet.cpp` directly with the fixed
test harness. Register `CASE(bsp,name)`, `CASE(rays,name)`, and
`CASE(animation,name)`, use `CHECK`, and do not provide a main function.
The future implementation is compiled from `src/cal3d/transparentpacket.cpp`.
There is no target implementation or target test in this starting snapshot.

The portable in-memory build uses upstream's `USE_CAL3D_WITH_CPP_11` switch
(std smart pointers/optional) and `IMVU_NO_INTRINSICS` scalar path. It does not
compile import/export code, so RapidXML, Boost, external TestFramework, and
graphics SDKs are not needed. `loader_sentinel.cpp` supplies only the exact
upstream InvalidTranslation constant needed to link the existing track code.

Legacy test bodies come directly from `testTriSort.cpp`, `testTransform.cpp`,
and `testPhysique.cpp`; only their unavailable TestPrologue include is replaced
in generated build copies. In scalar mode both skin fixtures call the x87
entry. Performance-only tests guarded by IMVU_NO_INTRINSICS remain excluded.
The wrapper verifies process exit status, positive case/check counts for each
group, group completion, and final completion outside the test process.
