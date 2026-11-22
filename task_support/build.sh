#!/bin/bash
set -euo pipefail
mkdir -p .task-build
incs=(-I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include)
opts=(-DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -D_LARGEFILE_SOURCE=1 -D_FILE_OFFSET_BITS=64)
if [[ $(uname -s) == Darwin ]]; then opts+=(-DBSD=1 -DHAVE_SOCKADDR_LEN=1); fi
for dir in UsageEnvironment BasicUsageEnvironment groupsock liveMedia; do
  mkdir -p ".task-build/$dir"
  find "$dir" -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' \) | LC_ALL=C sort > ".task-build/$dir/sources"
  test -s ".task-build/$dir/sources"
  while IFS= read -r src; do
    obj=".task-build/$dir/$(basename "$src").o"
    if [[ ! -f "$obj" || "$src" -nt "$obj" ]] || find "$dir/include" -name '*.hh' -newer "$obj" -print -quit | rg -q .; then
      case "$src" in
        *.cpp) c++ -std=c++20 -O1 "${incs[@]}" "${opts[@]}" -c "$src" -o "$obj" ;;
        *.c) cc -O1 "${incs[@]}" "${opts[@]}" -c "$src" -o "$obj" ;;
      esac
    fi
  done < ".task-build/$dir/sources"
  rm -f ".task-build/lib$dir.a"
  objects=(.task-build/"$dir"/*.o)
  ar rcs ".task-build/lib$dir.a" "${objects[@]}"
done
