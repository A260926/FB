#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
CXX=${CXX:-c++}
flags=(-std=c++11 -O2 -DFFTCONVOLVER_DONT_USE_SSE -I. -Isupport)
if [[ ${SANITIZE:-0} == 1 ]]; then flags+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); fi
sources=(AudioFFT.cpp FFTConvolver.cpp TwoStageFFTConvolver.cpp Utilities.cpp)
case ${1:-baseline} in
baseline)
  "$CXX" "${flags[@]}" "${sources[@]}" support/baseline.cpp -o .build/baseline
  .build/baseline
  ;;
legacy)
  "$CXX" "${flags[@]}" "${sources[@]}" test/Test.cpp -o .build/legacy
  .build/legacy > .build/legacy.stdout
  cat .build/legacy.stdout
  if grep -q '\[FAILED\]' .build/legacy.stdout; then exit 10; fi
  [[ $(grep -c '^Correctness Test.*\[OK\]$' .build/legacy.stdout) -eq 58 ]]
  echo 'LEGACY_COMPLETE cases=58'
  ;;
target)
  [[ -s test/scheduled_target.cpp ]] || { echo 'missing/empty test/scheduled_target.cpp' >&2; exit 11; }
  # All root production translation units, including newly added ones.
  production=(./*.cpp)
  "$CXX" "${flags[@]}" -include support/work_trace.h "${production[@]}" support/driver.cpp test/scheduled_target.cpp -o .build/target
  set +e
  .build/target > .build/target.stdout 2> .build/target.stderr
  status=$?
  set -e
  cat .build/target.stdout
  cat .build/target.stderr >&2
  [[ $status == 0 ]] || exit "$status"
  awk '
    /^CASE (pcm|schedule|lifecycle)\/[A-Za-z0-9_]+ checks=[1-9][0-9]* PASS$/ {split($2,a,"/");c[a[1]]++;next}
    /^GROUP (pcm|schedule|lifecycle) cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$/ {n=substr($3,7)+0;if(n<2 || n!=c[$2]) bad=1;g[$2]++;next}
    /^TARGET_COMPLETE cases=[1-9][0-9]*$/ {done++;next}
    END { if(bad || c["pcm"]<2 || c["schedule"]<2 || c["lifecycle"]<2 || g["pcm"]!=1 || g["schedule"]!=1 || g["lifecycle"]!=1 || done!=1) exit 12 }
  ' .build/target.stdout
  bash support/run.sh baseline
  bash support/run.sh legacy
  ;;
*) exit 64 ;;
esac
