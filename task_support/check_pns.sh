#!/usr/bin/env bash
# PNS content regression:
#   * builds the library with the test-only PNS_TEST_HOOK observation points
#   * builds tests/pns_reference.c + tests/pns_stream.c against it
#   * runs the numeric/state assertions
#   * then applies two source mutations in isolated copies and confirms the
#     same assertions reject both (a compile error does not count)
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$PWD"
WORK="$ROOT/.task-pns"
TOOL_BUILD="$WORK/tool"

# Keep the test translation unit ABI-compatible with the library build.
TEST_DEFINES=(
    -DPNS_TEST_HOOK
    -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 -DHAVE_STRING_H=1
    -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1
    -DAPPLY_DRC
)

rm -rf "$WORK"
mkdir -p "$TOOL_BUILD"

# Library with test hooks enabled.
cmake -S "$ROOT" -B "$TOOL_BUILD" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-DPNS_TEST_HOOK" >/dev/null
cmake --build "$TOOL_BUILD" --target faad -j 4 >/dev/null

# Reuse exactly the feature macros the library itself was compiled with.
LIB_DEFS=$(grep -o -- '-D[A-Z_]*\(=1\)\?' "$TOOL_BUILD/CMakeFiles/faad.dir/flags.make" | sort -u)
for d in $LIB_DEFS; do TEST_DEFINES+=("$d"); done

cc -std=c99 -O2 -Wall -Wextra "${TEST_DEFINES[@]}" \
    -Iinclude -I"$TOOL_BUILD/include" -Ilibfaad -Itests \
    tests/pns_reference.c tests/pns_stream.c \
    "$TOOL_BUILD/libfaad.a" -lm -o "$WORK/pns_stream"

echo "[1/3] reference + stream regression"
"$WORK/pns_stream"

# --- Mutation tests in isolated copies -------------------------------------
# Small, self-contained source rewriter built with the C compiler, so the
# script needs no tool beyond the compiler, CMake and the system math library.
MUTATOR="$WORK/mutator"
cat >"$WORK/mutator.c" <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long read_file(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc((size_t)n + 1);
    if (fread(*out, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -1; }
    (*out)[n] = 0;
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    char *s;
    const char *old, *new;
    long n;
    size_t count, oldlen, newlen;
    FILE *f;
    if (argc != 4) { fprintf(stderr, "usage: mutator file old new\n"); return 2; }
    n = read_file(argv[1], &s);
    if (n < 0) return 2;
    old = argv[2];
    new = argv[3];
    oldlen = strlen(old);
    newlen = strlen(new);
    count = 0;
    for (char *p = s; (p = strstr(p, old)) != NULL; p += oldlen)
        count++;
    if (count != 1) {
        fprintf(stderr, "expected exactly one injection site, found %zu\n",
            count);
        return 2;
    }
    {
        char *hit = strstr(s, old);
        char *result = malloc(strlen(s) - oldlen + newlen + 1);
        size_t head = (size_t)(hit - s);
        memcpy(result, s, head);
        memcpy(result + head, new, newlen);
        strcpy(result + head + newlen, hit + oldlen);
        f = fopen(argv[1], "wb");
        if (!f) { perror(argv[1]); return 2; }
        fwrite(result, 1, strlen(result), f);
        fclose(f);
    }
    return 0;
}
CEOF
cc -std=c99 -O2 -Wall -Wextra "$WORK/mutator.c" -o "$MUTATOR"

run_mutation () {
    local name="$1"; shift
    local dir="$WORK/$name"
    cp -R "$ROOT" "$dir"
    # Never recurse into scratch/build artefacts inside the copy.
    rm -rf "$dir/.task-build" "$dir/.task-pns" "$dir/.scratch"
    "$@" "$dir"
    local build="$dir/.mut-build"
    cmake -S "$dir" -B "$build" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_C_FLAGS="-DPNS_TEST_HOOK" >/dev/null
    if ! cmake --build "$build" --target faad -j 4 >"$WORK/$name.build.log" 2>&1; then
        echo "MUTATION $name: compile failed (does not count)" >&2
        cat "$WORK/$name.build.log" >&2
        return 1
    fi
    cc -std=c99 -O2 -Wall -Wextra "${TEST_DEFINES[@]}" \
        -I"$dir/include" -I"$build/include" -I"$dir/libfaad" -I"$dir/tests" \
        "$dir/tests/pns_reference.c" "$dir/tests/pns_stream.c" \
        "$build/libfaad.a" -lm -o "$dir/mut_test"
    if "$dir/mut_test" >"$WORK/$name.run.log" 2>&1; then
        echo "MUTATION $name: NOT rejected" >&2
        cat "$WORK/$name.run.log" >&2
        return 1
    fi
    echo "[ok] mutation $name rejected:"
    head -1 "$WORK/$name.run.log" | sed 's/^/    /'
}

# Mutation A: correlated right channel advances the global RNG anyway.
mutate_rng () {
    local dir="$1"
    "$MUTATOR" "$dir/libfaad/pns.c" \
        'gen_rand_vector(&spec_right[begin],
                            ics_right->scale_factors[g][sfb], end - begin, sub, &r1_dep, &r2_dep);' \
        'gen_rand_vector(&spec_right[begin],
                            ics_right->scale_factors[g][sfb], end - begin, sub, __r1, __r2);'
}

# Mutation B: short-window base ignores preceding groups.
mutate_base () {
    local dir="$1"
    "$MUTATOR" "$dir/libfaad/pns.c" \
        'uint16_t base = group * nshort;' \
        'uint16_t base = 0; /* MUTATION: ignore earlier groups */'
}

echo "[2/3] mutation: correlated right channel advances global RNG"
run_mutation mut_rng mutate_rng
echo "[3/3] mutation: short-window base ignores earlier groups"
run_mutation mut_base mutate_base

echo "PNS_CHECK_OK"
