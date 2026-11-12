mkdir -p "$build/include/yajl"
cp src/api/*.h "$build/include/yajl/"
sed -e 's/${YAJL_MAJOR}/2/g' -e 's/${YAJL_MINOR}/1/g' -e 's/${YAJL_MICRO}/1/g' src/api/yajl_version.h.cmake > "$build/include/yajl/yajl_version.h"
for src in src/*.c; do
 cc -std=c99 -O1 -D_GNU_SOURCE -DYAJL_BUILD -I"$build/include" -Isrc -c "$src" -o "$build/$(basename "$src" .c).o"
done
