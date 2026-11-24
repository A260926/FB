# Linux compiler toolchain

The task and upstream model-checker/fiber runtime require Linux. On Linux,
`bash task_support/run.sh baseline` and `target` use the system compiler directly.
On macOS the same entry point dispatches to a pre-provisioned standard Linux GCC
14.2.0 SDK image with immutable ID `sha256:b99b86a28812b1e6453a231a947dc43d76fe192788a12f344a9b568bf9f5d24c` (linux/arm64).
The source uses no downloadable application packages. The optional macOS path
requires Docker and that compiler image, and deliberately never pulls it.
Image preparation is a separate prerequisite, before offline verification; the
source package does not contain the SDK. A matching image can be provisioned
from `gcc@sha256:b99b86a28812b1e6453a231a947dc43d76fe192788a12f344a9b568bf9f5d24c` and its image ID must be checked before use.

Every compiler-container invocation uses `--pull=never --network=none`, mounts
only the restored source tree, and mounts no host package or build caches.
This is an offline application build with a supplied compiler SDK, not a claim
that an empty Docker store can build it. The macOS native build failure is a platform limit,
not a discovered defect.
`target` executes the real target entry point; it is expected to fail until the
requested feature and tests are implemented. No success is substituted for it.
