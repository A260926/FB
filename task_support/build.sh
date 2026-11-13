#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
sed 's/@LISTHASH_PREFIX@/libtar/g' listhash/listhash.h.in > .task-build/libtar_listhash.h
sed 's/@LISTHASH_PREFIX@/libtar/g' listhash/list.c.in > .task-build/list.c
sed 's/@LISTHASH_PREFIX@/libtar/g' listhash/hash.c.in > .task-build/hash.c
compat_sources=""
if [ "$(uname -s)" != Darwin ]; then compat_sources="compat/strlcpy.c compat/strlcat.c"; fi
for src in lib/*.c .task-build/list.c .task-build/hash.c $compat_sources; do
 obj=.task-build/$(basename "$src" .c).o
 cc -std=gnu99 -O1 -g ${CHECK_SANITIZERS:-} -Itask_support -Ilib -Icompat -I.task-build -c "$src" -o "$obj"
done
ar rcs .task-build/libtar.a .task-build/*.o
