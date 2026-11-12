#!/bin/bash
set -euo pipefail
test -s test/org/netpreserve/jwarc/WarcReassemblyReaderTest.java
bash regress/maven.sh -q -Dtest=WarcReassemblyReaderTest -DfailIfNoTests=true -Dsurefire.failIfNoSpecifiedTests=true test
r=target/surefire-reports/TEST-org.netpreserve.jwarc.WarcReassemblyReaderTest.xml
test -s "$r"
grep -Eq 'tests="[1-9][0-9]*"' "$r"
for x in 'skipped="0"' 'failures="0"' 'errors="0"'; do grep -q "$x" "$r"; done
printf 'WARC_REASSEMBLY_TARGET_OK\n'
bash regress/maven.sh -q -Dtest=BaselineTest,WarcRecordTest,LengthedBodyTest -DfailIfNoTests=true test
