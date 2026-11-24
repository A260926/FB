#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/qpack_model_test.c; fi
mkdir -p .task-build/include/nghttp3
printf '#define HAVE_ARPA_INET_H 1\n#define HAVE_NETINET_IN_H 1\n' > .task-build/config.h
sed -e 's/@PACKAGE_VERSION@/1.19.90/g' -e 's/@PACKAGE_VERSION_NUM@/0x01135a/g' lib/includes/nghttp3/version.h.in > .task-build/include/nghttp3/version.h
for unit in nghttp3_balloc nghttp3_buf nghttp3_callbacks nghttp3_conn nghttp3_conv nghttp3_debug nghttp3_err nghttp3_frame nghttp3_gaptr nghttp3_http nghttp3_idtr nghttp3_ksl nghttp3_map nghttp3_mem nghttp3_objalloc nghttp3_opl nghttp3_pq nghttp3_qpack nghttp3_qpack_huffman nghttp3_qpack_huffman_data nghttp3_range nghttp3_ratelim nghttp3_rcbuf nghttp3_ringbuf nghttp3_settings nghttp3_str nghttp3_stream nghttp3_tnode nghttp3_unreachable nghttp3_vec nghttp3_version; do
 cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. -c "lib/$unit.c" -o ".task-build/$unit.o"
done
cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. -c lib/sfparse/sfparse.c -o .task-build/sfparse.o
ar rcs .task-build/libnghttp3.a .task-build/nghttp3_balloc.o .task-build/nghttp3_buf.o .task-build/nghttp3_callbacks.o .task-build/nghttp3_conn.o .task-build/nghttp3_conv.o .task-build/nghttp3_debug.o .task-build/nghttp3_err.o .task-build/nghttp3_frame.o .task-build/nghttp3_gaptr.o .task-build/nghttp3_http.o .task-build/nghttp3_idtr.o .task-build/nghttp3_ksl.o .task-build/nghttp3_map.o .task-build/nghttp3_mem.o .task-build/nghttp3_objalloc.o .task-build/nghttp3_opl.o .task-build/nghttp3_pq.o .task-build/nghttp3_qpack.o .task-build/nghttp3_qpack_huffman.o .task-build/nghttp3_qpack_huffman_data.o .task-build/nghttp3_range.o .task-build/nghttp3_ratelim.o .task-build/nghttp3_rcbuf.o .task-build/nghttp3_ringbuf.o .task-build/nghttp3_settings.o .task-build/nghttp3_str.o .task-build/nghttp3_stream.o .task-build/nghttp3_tnode.o .task-build/nghttp3_unreachable.o .task-build/nghttp3_vec.o .task-build/nghttp3_version.o .task-build/sfparse.o
cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. task_support/baseline.c .task-build/libnghttp3.a -o .task-build/baseline
if [[ $mode == target ]]; then
  for required in fuzz/stateful/model.h fuzz/stateful/model.c fuzz/stateful/transport.c fuzz/stateful/replay.c fuzz/stateful/main.c examples/qpack_schedule.txt; do test -s "$required"; done
for unit in model transport replay; do
 cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. -include fuzz/stateful/model.h -c "fuzz/stateful/$unit.c" -o ".task-build/stateful_$unit.o"
done
cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. task_support/api_probe.c .task-build/stateful_model.o .task-build/stateful_transport.o .task-build/stateful_replay.o .task-build/libnghttp3.a -o .task-build/api_probe
.task-build/api_probe
cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. fuzz/stateful/main.c .task-build/stateful_model.o .task-build/stateful_transport.o .task-build/stateful_replay.o .task-build/libnghttp3.a -o .task-build/replay_cli
rm -f .task-build/example.out
.task-build/replay_cli < examples/qpack_schedule.txt > .task-build/example.out
test -s .task-build/example.out
grep -qx 'CONSISTENT' .task-build/example.out
cc -std=c11 -O0 -g -DHAVE_CONFIG_H -DNGHTTP3_STATICLIB -I.task-build -I.task-build/include -Ilib/includes -Ilib -I. task_support/driver.c tests/qpack_model_test.c .task-build/stateful_model.o .task-build/stateful_transport.o .task-build/stateful_replay.o .task-build/libnghttp3.a -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/qpack-model-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sections feedback cancel replay; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
