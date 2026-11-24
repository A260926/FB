#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
if [[ $mode == target ]]; then
  for source in fuzz/reassembly.h fuzz/reassembly.c fuzz/fuzz_reassembly.cc fuzz/replay_reassembly.c; do test -s "$source"; done
fi
task_build=$(mktemp -d "${TMPDIR:-/tmp}/ngtcp2-task.XXXXXX")
trap 'rm -rf "$task_build"' EXIT
mkdir -p "$task_build/include/ngtcp2"
sed -e 's/@PACKAGE_VERSION@/1.26.90/g' -e 's/@PACKAGE_VERSION_NUM@/0x011a5a/g' lib/includes/ngtcp2/version.h.in > "$task_build/include/ngtcp2/version.h"
runtime_objects=()
for source in vendor/libfuzzer/Fuzzer*.cpp; do
  object="$task_build/$(basename "$source" .cpp).o"
  clang++ -std=c++17 -g -O1 -pthread -c "$source" -o "$object"
  runtime_objects+=("$object")
done
test "${#runtime_objects[@]}" -eq 26
ar rcs "$task_build/libFuzzer.a" "${runtime_objects[@]}"
for pool in pooled direct; do
  mkdir -p "$task_build/$pool/corpus"
  flags=(-g -O1 -fno-omit-frame-pointer -fsanitize=fuzzer-no-link,address,undefined -DNGTCP2_STATICLIB -Ilib/includes -Ilib -I"$task_build/include")
  if [[ $pool == direct ]]; then flags+=(-DNOMEMPOOL); fi
  objects=()
  while IFS= read -r source; do
    test -s "$source"
    object="$task_build/$pool/$(basename "$source" .c).o"
    clang -std=c11 "${flags[@]}" -c "$source" -o "$object"
    objects+=("$object")
  done < task_support/library_sources.txt
  test "${#objects[@]}" -eq 46
  ar rcs "$task_build/$pool/libngtcp2.a" "${objects[@]}"
  clang -std=c11 "${flags[@]}" -c task_support/baseline.c -o "$task_build/$pool/baseline.o"
  clang++ "${flags[@]}" "$task_build/$pool/baseline.o" "$task_build/$pool/libngtcp2.a" "$task_build/libFuzzer.a" -o "$task_build/$pool/baseline"
  "$task_build/$pool/baseline"
  clang -std=c11 "${flags[@]}" -DBASELINE_FUZZ -c task_support/baseline.c -o "$task_build/$pool/base-fuzz.o"
  clang++ "${flags[@]}" "$task_build/$pool/base-fuzz.o" "$task_build/$pool/libngtcp2.a" "$task_build/libFuzzer.a" -o "$task_build/$pool/baseline_fuzz"
  "$task_build/$pool/baseline_fuzz" -seed=35 -runs=8 -max_len=1 -timeout=5 > "$task_build/$pool/base-fuzz.log" 2>&1
  cat "$task_build/$pool/base-fuzz.log"
  grep -Eq 'cov: [1-9][0-9]*' "$task_build/$pool/base-fuzz.log"
  echo "BASELINE pool=$pool instrumented=ASan+UBSan+libFuzzer healthy_runs=8 coverage=present"
  if [[ $mode == target ]]; then
    clang -std=c11 "${flags[@]}" -Ifuzz -include fuzz/reassembly.h -c fuzz/reassembly.c -o "$task_build/$pool/state.o"
    clang -std=c11 "${flags[@]}" -Ifuzz -c fuzz/replay_reassembly.c -o "$task_build/$pool/replay.o"
    clang++ -std=c++17 "${flags[@]}" -Ifuzz -c fuzz/fuzz_reassembly.cc -o "$task_build/$pool/fuzz.o"
    clang -std=c11 "${flags[@]}" -Ifuzz -c task_support/api_probe.c -o "$task_build/$pool/probe.o"
    clang++ "${flags[@]}" "$task_build/$pool/probe.o" "$task_build/$pool/state.o" "$task_build/$pool/libngtcp2.a" "$task_build/libFuzzer.a" -o "$task_build/$pool/probe"
    "$task_build/$pool/probe"
    clang++ "${flags[@]}" "$task_build/$pool/replay.o" "$task_build/$pool/state.o" "$task_build/$pool/libngtcp2.a" "$task_build/libFuzzer.a" -o "$task_build/$pool/replay"
    clang++ "${flags[@]}" "$task_build/$pool/fuzz.o" "$task_build/$pool/state.o" "$task_build/$pool/libngtcp2.a" "$task_build/libFuzzer.a" -o "$task_build/$pool/fuzz"
    nm "$task_build/$pool/fuzz" | grep -E ' [Tt] _?LLVMFuzzerCustomMutator$' > "$task_build/$pool/mutator-symbol.txt"
    for name in empty body pressure cancel split; do
      "$task_build/$pool/replay" "task_support/corpus/$name" > "$task_build/$pool/$name.1"
      "$task_build/$pool/replay" "task_support/corpus/$name" > "$task_build/$pool/$name.2"
      test -s "$task_build/$pool/$name.1"
      cmp "$task_build/$pool/$name.1" "$task_build/$pool/$name.2"
      cp "task_support/corpus/$name" "$task_build/$pool/corpus/$name"
    done
    "$task_build/$pool/fuzz" -seed=35 -runs=256 -max_len=1157 -timeout=5 "$task_build/$pool/corpus" > "$task_build/$pool/fuzz.log" 2>&1
    cat "$task_build/$pool/fuzz.log"
    grep -Eq 'cov: [1-9][0-9]*' "$task_build/$pool/fuzz.log"
    grep -q 'LLVMFuzzerCustomMutator' "$task_build/$pool/fuzz.log"
    echo "TARGET_COMPLETE pool=$pool replays=10 structured_fuzz_runs=256"
  fi
done
