The task environment limits the Cargo workspace to the real cavalier_contours core crate.
Its production Rust files, features and existing integration tests are unchanged. The
Criterion dev dependency and benchmark target declaration were removed from the core
manifest so QA does not resolve benchmark/UI/FFI dependencies. Those source directories
remain available as upstream context but are outside this workspace build.

vendor/ contains the 17 crates resolved by Cargo.lock (including the existing optional
serde feature). Original license files and Cargo checksums are retained. Project-local
.cargo/config.toml replaces crates.io with that directory and forces offline operation.
No global Cargo configuration changes are needed. A Rust toolchain >=1.88 is required;
there are no services, accounts, online fixtures or runtime downloads.

From the repository root: bash checks/run-baseline.sh
This uses an empty temporary CARGO_HOME and --frozen. The target command is
bash checks/check-offset-reference.sh; its new reference tests and mutant script must
be written before it can succeed. Compilation alone or skipped tests do not pass.
