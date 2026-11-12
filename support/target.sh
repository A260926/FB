#!/bin/bash
set -euo pipefail
test -s tests/multiget.cpp || { echo 'missing/empty tests/multiget.cpp' >&2; exit 2; }
b=${CHECK_BUILD_DIR:-$(mktemp -d "$PWD/.build.XXXXXX")}
cmake -S support -B "$b" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$b" --target target -j2
(cd "$b"; ./target) | tee "$b/target.log"
awk '
BEGIN {split("versions routing blocks errors",names," ")}
/^MULTIGET_GROUP / {
 if ($0 !~ /^MULTIGET_GROUP (versions|routing|blocks|errors) PASS cases=[1-9][0-9]* checks=[1-9][0-9]*$/) exit 2;
 ++groups; if(groups>4 || $2!=names[groups] || complete) exit 2;
 split($4,a,"="); split($5,b,"="); cases+=a[2]; checks+=b[2];
}
/^MULTIGET_COMPLETE / {
 if($0 !~ /^MULTIGET_COMPLETE groups=4 cases=[1-9][0-9]* checks=[1-9][0-9]*$/) exit 2;
 split($3,a,"="); split($4,b,"=");
 if(groups!=4 || complete || a[2]!=cases || b[2]!=checks) exit 2;
 complete=1;
}
END {if(groups!=4 || !complete) exit 2}
' "$b/target.log" || { echo 'incomplete target execution' >&2; exit 2; }
CHECK_BUILD_DIR="$b" bash support/baseline.sh
