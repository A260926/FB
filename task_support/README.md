CPU build support

Run `bash task_support/run.sh baseline` with Rust 1.94.0 installed.
`target` additionally compiles task_support/multipatch_tests.rs inside the
mesh::octree::task_target module (the file is to be added by the contributor).
Register `const CASES: &[(&str, &str, fn(&mut Check))]`; each callback must call
`c.check(condition, message)` on the actual result. Group names are topology,
qef, seams, compat. No skip protocol is provided. Empty groups, no assertions,
panics, and process exits before the driver completion cannot succeed.
These checks establish that callbacks ran; their content must test the requested
geometry, not just the driver. The supplied baseline has no multi-patch solution.

Source: https://github.com/mkeeter/fidget, v0.3.9,
cc686591cbd45644a998df2da2fd4dd2eedc6bab (MPL-2.0, LICENSE.txt).
The workspace is exported for CPU fidget + workspace-hack only. Root and fidget
manifests omit GUI/CLI demos, optional JIT/Rhai/wasm/windows bindings and criterion
benchmarks; all actual CPU production dependencies, approx tests and source
models remain. The supported build uses --no-default-features; enabling jit/rhai
is outside this export. Cargo.lock preserves exact versions/checksums from the
upstream lock and prunes unreachable entries. Cargo vendor includes every
resolved registry package, checksum metadata and original license material.
The per-target production dependency counts are 104 on aarch64-apple-darwin,
94 on x86_64-unknown-linux-gnu and 94 on aarch64-unknown-linux-gnu. The complete
lock has 141 registry packages, including target-only/dev/transitive entries;
these counts must not be described as zero dependencies. Target-list resolution
and source completeness do not establish a Linux compile or test result.

Rust 1.94.0 is pinned because the original library tests compiled and ran there;
the preparation run on 1.98.1 failed with E0689 (ambiguous x.sin() in
fidget/src/core/eval/test/point.rs:241). No upstream test or solver was patched
to hide that failure. Cargo is offline, uses vendor, and supports empty caches.
seed-export.json records original/adapted files; dependencies.json records
package versions, archive checksums and license locations. Apart from a cfg(test)
support hook appended to octree.rs, the production Rust sources are unmodified.

Some original crate archives omit license bodies. third-party-licenses/manifest.json
maps these 13 packages to supplements: ten use license files from exact release
VCS commits; three offer Apache-2.0 in original metadata and use the unmodified
standard Apache text from apache.org. Original copyright/SPDX/source notices
remain in vendor; no crate archive or checksum metadata was altered.
