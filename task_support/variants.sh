#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/variants
bash task_support/run.sh target > .task-build/variants/original.log 2>&1
echo ORIGINAL_CONTENT_OK
variant_root=$(mktemp -d)
trap 'rm -rf "$variant_root"' EXIT
for variant in accept_crc orphan_suffix peek_hole; do
  dest="$variant_root/$variant"
  mkdir -p "$dest"
  tar --exclude=.git --exclude=.task-build -cf - . | tar -xf - -C "$dest"
  awk -v variant="$variant" '
    {line=$0}
    /Bad checksum. Lose sync/ {crc=1}
    variant=="accept_crc" && crc && /goto sync_fail;/ {sub(/goto sync_fail;/,"(void)0;",line);n++;crc=0}
    variant=="orphan_suffix" && /if\(continued\)\{/ {line="  if(continued && 0){";n++}
    /if\(os->lacing_vals\[ptr\]\&0x400\)/ {hole=1}
    variant=="peek_hole" && hole && /os->lacing_returned\+\+;/ {sub(/os->lacing_returned\+\+;/,"if(adv)os->lacing_returned++;",line);n++}
    variant=="peek_hole" && hole && /os->packetno\+\+;/ {sub(/os->packetno\+\+;/,"if(adv)os->packetno++;",line)}
    /return\(-1\);/ {hole=0}
    {print line}
    END {if(n!=1)exit 9}
  ' src/framing.c > "$dest/src/framing.c"
  (cd "$dest"; cmake -S . -B .task-build/build -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF;
    cmake --build .task-build/build --parallel 2;
    cc -std=c11 -Wall -Wextra -I. -Iinclude -I.task-build/build/include task_support/driver.c tests/recovery_cases.c .task-build/build/libogg.a -o .task-build/target) > ".task-build/variants/$variant-compile.log" 2>&1
  set +e
  (cd "$dest"; .task-build/target) > ".task-build/variants/$variant.log" 2>&1
  status=$?
  set -e
  [[ $status == 4 ]]
  grep -Eq '^FAIL tests/recovery_cases.c:[0-9]+ ' ".task-build/variants/$variant.log"
  echo "MUTANT_CONTENT_REJECTED $variant"
done
echo OGG_VARIANTS_COMPLETE
