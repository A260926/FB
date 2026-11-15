#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/cache-edit.c; fi
mkdir -p .task-build/plain .task-build/thread
task_sources=(src/anim.c src/track.c src/dynarr.c)
for task_required in "${task_sources[@]}"; do test -s "$task_required"; done
while IFS= read -r task_extra; do
 case "$task_extra" in src/anim.c|src/track.c|src/dynarr.c) ;; *) task_sources+=("$task_extra");; esac
done < <(find src -type f -name '*.c' | LC_ALL=C sort)
for task_variant in plain thread; do
 task_flags=(-std=c99 -O1 -I. -Isrc)
 if [[ $task_variant == thread ]]; then task_flags+=(-DANIM_CONFIG_H_ -DANIM_THREAD_SAFE -pthread); fi
 task_objects=()
 for task_src in "${task_sources[@]}"; do
  task_object=".task-build/$task_variant/${task_src//\//_}.o"
  cc "${task_flags[@]}" -c "$task_src" -o "$task_object"
  task_objects+=("$task_object")
 done
 rm -f ".task-build/$task_variant/libanim.a"
 ar rcs ".task-build/$task_variant/libanim.a" "${task_objects[@]}"
 cc "${task_flags[@]}" baseline/normal.c ".task-build/$task_variant/libanim.a" -lm -o ".task-build/$task_variant/normal"
 if [[ $mode == baseline ]]; then
  cc "${task_flags[@]}" baseline/repro.c ".task-build/$task_variant/libanim.a" -lm -o ".task-build/$task_variant/repro"
 fi
done
if [[ $mode == target ]]; then
  test -s example/cache-edit-cpu.c
for task_variant in plain thread; do
 task_flags=(-std=c99 -O1 -I. -Isrc)
 if [[ $task_variant == thread ]]; then task_flags+=(-DANIM_CONFIG_H_ -DANIM_THREAD_SAFE -pthread); fi
 cc "${task_flags[@]}" task_support/driver.c tests/cache-edit.c ".task-build/$task_variant/libanim.a" -lm -o ".task-build/$task_variant/target"
 cc "${task_flags[@]}" example/cache-edit-cpu.c ".task-build/$task_variant/libanim.a" -lm -o ".task-build/$task_variant/example"
done
cat > .task-build/target <<'RUN'
#!/bin/bash
set -euo pipefail
for task_variant in plain thread; do
 .task-build/$task_variant/target
 rm -f ".task-build/$task_variant/example.log"
 .task-build/$task_variant/example >".task-build/$task_variant/example.log" 2>&1
 cat ".task-build/$task_variant/example.log"
 grep -qx CACHE_EDIT_DEMO_OK ".task-build/$task_variant/example.log"
done
RUN
chmod +x .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in track_edits ownership thread_cache; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/plain/normal
.task-build/thread/normal
if [[ $mode == baseline ]]; then
 .task-build/plain/repro
 .task-build/thread/repro
fi
