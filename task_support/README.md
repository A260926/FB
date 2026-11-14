libelf/ and common/ are original exports from the fixed Elftoolchain revision.
The root LICENSE and all per-file notices are retained. upstream-tests/ contains
original archive-related TET specifications for reference; the external TET
harness is not a runtime dependency and is not run by this support wrapper.

The three generated libelf C files and sys/elfdefinitions.h are outputs from the
upstream m4 sources. Their original sources remain included. They are delivered
as ordinary source files, so the build needs only cc, ar and bash. Optional
regenerate.sh needs system m4; it supplies no thin-archive implementation.

build.sh compiles every current libelf/*.c plus the generated conversion code,
then links the baseline against that library. Added production C files under
libelf/ therefore enter the actual link. Target mode requires tests/thin_contract.c,
uses the shared executed-assertion driver, and then reruns the regular baseline.
Run scripts from the seed root with bash task_support/run.sh baseline or target.

make_samples.c creates fixed ELF64 ET_REL objects with alpha and beta definitions,
and normal/thin GNU archives with an inline symbol index and long-name table.
The objects are never linked into a process or executed. The thin archive's last
member header ends exactly at EOF; its ar_size describes an external 512-byte
ELF object, not an in-archive payload. Object paths retain the objects/ directory.
To regenerate samples after building: .task-build/make_samples

The baseline reads the normal archive index, seeks members, decodes their ELF
symbol definitions, and compares the names. --probe additionally reports the
currently observed thin archive kind. No resolver implementation or target
regression tests are provided.
