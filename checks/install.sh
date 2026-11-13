set -eu
cd "$(dirname "$0")/.."
ac_cv_func_sbrk=no ac_cv_func_mmap=no ./configure --disable-shlib --disable-cxx --disable-threads
sed 's/^#define INTERNAL_MEMORY_SPACE .*/#define INTERNAL_MEMORY_SPACE (16 * 1024 * 1024)/' settings.h > settings.h.new
mv settings.h.new settings.h
make -j2 libdmalloc.a
