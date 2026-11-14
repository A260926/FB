Offline inputs

Upstream: https://github.com/google/goexpect @ ab937bf7fd6f93d0b1adaef7bf038f31f61bc96c
Original LICENSE and tests retained. go mod tidy only reconciled the existing module graph; vendor contains full Go sources for imported package closure and licenses. Versions in go.mod, go.sum and vendor/modules.txt; no runtime downloads.

Go vet is disabled for the legacy non-constant format string in NewStatusf; production code is not changed to satisfy new toolchain vet diagnostics.
