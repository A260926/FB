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

## TransparentPacket example

`cal3d::TransparentPacket` physically splits intersecting/cycling triangles into
a BSP once, then answers back-to-front draw orders for any number of cameras.
Build it once for a fixed pose; only call `build` again when the pose, morph
weights or object-to-world transform change. Moving the camera only needs
`order`, which never re-skins or re-splits and never touches the source mesh.

```cpp
#include "cal3d/transparentpacket.h"

cal3d::TransparentPacket packet;

// After updating the skeleton palette and any morph weights for this frame:
cal3d::TransparentInput inputs[2];
inputs[0].submesh       = shirt;                 // const CalSubmesh*
inputs[0].bones         = palette;               // current-pose skin palette
inputs[0].boneCount     = boneCount;
inputs[0].objectToWorld = shirtObjectToWorld;    // unit-quaternion rigid transform
inputs[1]               = inputs[0];
inputs[1].objectToWorld = coatObjectToWorld;     // all instances share one BSP

// Hard cap on stored fragment triangles; an overshoot fails instead of
// truncating. A failed build leaves the previous good packet untouched.
if (!packet.build(inputs, 2, 1e-4f, 20000)) {
    // keep using the previous packet (or skip the frame)
}

// Per camera: the source submesh/palette may even have been released by now.
std::vector<size_t> drawOrder;
if (packet.order(cameraEyeWorld, drawOrder)) {
    for (size_t fragmentIndex : drawOrder) {      // already far to near
        const auto& fragment = packet.fragments()[fragmentIndex];
        drawTransparent(fragment);               // plain source-over blending
    }
}

// Rebuild only when geometry actually changes (new pose, morph weight, or
// objectToWorld). Camera moves reuse the same packet via order().
```

