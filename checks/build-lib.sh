# Source with a temporary build directory already set.
cat > "$build/config.h" <<'CONFIG'
#define HAVE_WCHAR_H 1
#if defined(__APPLE__)
#define HAVE_XLOCALE_H 1
#define HAVE_WCSTOMBS_L 1
#endif
#define PACKAGE_VERSION "1.6.4"
CONFIG
for src in src/xls.c src/ole.c src/endian.c src/locale.c src/xlstool.c src/formula_deps.c; do
 [[ -f "$src" ]] || continue
 cc -std=gnu99 -O1 -I"$build" -Iinclude -c "$src" -o "$build/$(basename "$src" .c).o"
done
