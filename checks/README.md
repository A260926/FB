# Offline starting environment

The unmodified CharLS codec comes from team-charls/charls at e682efabdf5f55bb6758b56902dd741958db0e70 (BSD-3-Clause).
GoogleTest is supplied as C++ source at checks/vendor/googletest; PROVENANCE.json lists the pinned commit and hashes. No package manager or download is used.
The original tests are compiled unchanged. Only their build wiring selects the bundled test framework.
Required tools: CMake >=3.22, a C++17 compiler and its standard library, and a local build backend (Make or Ninja). Python is not used to build or run these checks.

Run bash checks/install.sh, then bash checks/baseline.sh. The baseline tests existing whole-frame behavior and selected upstream tests; it does not provide the streaming implementation or its requested tests.
The target runner compiles test/stream_decoder_contract_test.cpp and checks actual GoogleTest results for all four named suites.
