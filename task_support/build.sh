#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/include/libelf .task-build/obj .task-build/samples
cp lib/*.h .task-build/include/libelf/
cp task_support/config/config.h .task-build/include/
cp task_support/config/sys_elf.h .task-build/include/libelf/
cp task_support/config/sys_elf.h .task-build/include/
compiler=${CC:-cc}
cat > .task-build/profile.c <<'PROFILE'
#include <limits.h>
_Static_assert(CHAR_BIT == 8 && sizeof(short) == 2 && sizeof(int) == 4 && sizeof(long) == 8, "support profile requires POSIX LP64");
PROFILE
"$compiler" -std=c11 -c .task-build/profile.c -o .task-build/profile.o
objects=()
for source in lib/*.c; do
  object=".task-build/obj/${source##*/}.o"
  "$compiler" -std=c11 -O0 -g -DHAVE_CONFIG_H=1 -I.task-build/include -Ilib -c "$source" -o "$object"
  objects+=("$object")
done
if [[ -f .task-build/libelf-task.a ]]; then rm -- .task-build/libelf-task.a; fi
ar rcs .task-build/libelf-task.a "${objects[@]}"
"$compiler" -std=c11 -O0 -I.task-build/include -Ilib -Itask_support task_support/fixture.c task_support/generate.c -o .task-build/generate
.task-build/generate .task-build/samples
"$compiler" -std=c11 -O0 -I.task-build/include -Ilib -Itask_support task_support/fixture.c task_support/baseline.c .task-build/libelf-task.a -o .task-build/baseline
