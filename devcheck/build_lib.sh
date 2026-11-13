#!/bin/sh
#
# Build the real xvidcore sources with nothing but a C compiler.
#
# Xvid's autoconf build injects a C23 dialect on some toolchains, which trips
# over the legacy "typedef int bool" in encoder.h. The dev checks only need
# the plain C library, so they compile the documented generic source list
# directly. Everything lands in a scratch directory next to this script.
#
# Usage: build_lib.sh <output-dir>
# On success prints the path of the static library on stdout.

set -e

here=`dirname "$0"`
root=`cd "$here/.." && pwd`
out="$1"

if [ -z "$out" ]; then
	echo "usage: $0 <output-dir>" >&2
	exit 2
fi

cc=${CC:-cc}
src="$root/xvidcore/src"
sources_inc="$root/xvidcore/build/generic/sources.inc"

if [ ! -f "$sources_inc" ]; then
	echo "missing $sources_inc" >&2
	exit 2
fi

mkdir -p "$out/obj"

# Detect pointer size and byte order with the compiler itself.
cat > "$out/probe.c" <<'EOF'
#if defined(__SIZEOF_POINTER__)
#if __SIZEOF_POINTER__ == 8
#error XVID_BUS_64
#else
#error XVID_BUS_32
#endif
#endif
EOF

bus="64BIT"
if "$cc" -E "$out/probe.c" >/dev/null 2>"$out/probe.err"; then
	bus="64BIT"
elif grep -q XVID_BUS_32 "$out/probe.err"; then
	bus="32BIT"
fi

cat > "$out/endian.c" <<'EOF'
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error XVID_BIG
#else
#error XVID_LITTLE
#endif
#endif
EOF

endian="LITTLE_ENDIAN"
if "$cc" -E "$out/endian.c" >/dev/null 2>"$out/endian.err"; then
	endian="LITTLE_ENDIAN"
elif grep -q XVID_BIG "$out/endian.err"; then
	endian="BIG_ENDIAN"
fi

cflags="-O2 -fPIC -DARCH_IS_$bus -DARCH_IS_GENERIC -DARCH_IS_$endian -I$src"

# Collect the generic C sources from the canonical source list.
list=`sed -n '/^SRC_GENERIC =/,/^$/p' "$sources_inc" | tr ' \\' '\n' | grep '\.c$'`

objs=""
for f in $list; do
	obj="$out/obj/`echo "$f" | tr / _`.o"
	"$cc" $cflags -c "$src/$f" -o "$obj"
	objs="$objs $obj"
done

lib="$out/libxvidcore.a"
rm -f "$lib"
ar rc "$lib" $objs
echo "$lib"
