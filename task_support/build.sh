#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cat > .task-build/rtpconfig_unix.h <<'CFG'
#ifndef RTPCONFIG_UNIX_H
#define RTPCONFIG_UNIX_H
#define RTP_SUPPORT_SDESPRIV
#define RTP_SOCKLENTYPE_UINT
#define RTP_SUPPORT_PROBATION
#define RTP_SUPPORT_INLINETEMPLATEPARAM
#if defined(__APPLE__)
#define RTP_HAVE_SOCKADDR_LEN
#define RTP_FILIO
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define RTP_BIG_ENDIAN
#endif
#endif
CFG
printf '#include <stdint.h>\n#include <sys/types.h>\n' > .task-build/rtptypes_unix.h
rm -f .task-build/rtp-*.o .task-build/libproduction.a .task-build/baseline
for unit in src/*.cpp; do
 test -s "$unit"
 c++ -std=c++11 -O0 -I.task-build -Isrc -c "$unit" -o ".task-build/rtp-$(basename "$unit" .cpp).o"
done
ar rcs .task-build/libproduction.a .task-build/rtp-*.o
c++ -std=c++11 -I.task-build -Isrc task_support/baseline.cpp .task-build/libproduction.a -o .task-build/baseline
