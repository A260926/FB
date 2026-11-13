#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .output
# Fixed driver: the task supplies callbacks, not a replacement executable/runner.
cc -std=c99 -O2 -Isupport support/impl.c support/marker_main.c tests/marker_contract.c -lm -o .output/marker_contract
set +e
.output/marker_contract > .output/marker_contract.stdout 2> .output/marker_contract.stderr
status=$?
set -e
cat .output/marker_contract.stdout
cat .output/marker_contract.stderr >&2
if [ "$status" -ne 0 ]; then exit "$status"; fi
awk '
BEGIN { n=split("topology coordinates paint reuse",a," ");for(i=1;i<=n;i++)want[a[i]]=1 }
$1=="MARKER_GROUP" && NF==6 && $6=="DONE" {
 if(!($2 in want) || seen[$2]++)exit 2;
 if($3!~/^cases=[1-9][0-9]*$/ || $4!~/^checks=[1-9][0-9]*$/ || $5!="failures=0")exit 2;
}
$1=="MARKER_ALL_DONE" && NF==2 && $2~/^checks=[1-9][0-9]*$/ {done++}
END {for(i=1;i<=n;i++)if(seen[a[i]]!=1)exit 3;if(done!=1)exit 3}
' .output/marker_contract.stdout
bash support/baseline.sh --legacy
