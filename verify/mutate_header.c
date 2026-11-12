/*
 * mutate_header.c - behavior-changing mutation injector for the octasphere
 * geometry regression harness.
 *
 * This is NOT a detector: it only produces a temporary, mutated copy of
 * par_octasphere.h so that verify/octasphere_geometry.cpp can prove that its
 * geometric assertions actually catch real regressions. Each replacement is
 * an exact, single-occurrence source edit; if the anchor text is missing or
 * ambiguous the tool fails loudly instead of silently producing a no-op copy.
 *
 * Usage: mutate_header <angle|offset|winding> <input.h> <output.h>
 *   angle   : multiply the interior geodesic rotation angle by 0.8
 *   offset  : cancel the rounded-box Z axis patch translation
 *   winding : reverse the first triangle inside each connector quad
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* slurp(const char* path, size_t* length) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mutate_header: cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fprintf(stderr, "mutate_header: empty or unreadable %s\n", path);
        exit(2);
    }
    char* buf = (char*)malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "mutate_header: short read on %s\n", path);
        exit(2);
    }
    fclose(f);
    buf[n] = '\0';
    *length = (size_t)n;
    return buf;
}

static size_t count_occurrences(const char* hay, const char* needle) {
    size_t count = 0;
    const char* p = hay;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        ++count;
        p += nlen;
    }
    return count;
}

static char* replace_once(const char* src, const char* anchor, const char* repl,
                          const char* mode) {
    size_t hits = count_occurrences(src, anchor);
    if (hits != 1) {
        fprintf(stderr,
                "mutate_header: mode '%s' expected exactly one anchor, found %zu\n",
                mode, hits);
        exit(3);
    }
    size_t cap = strlen(src) + strlen(repl) + 1;
    char* out = (char*)malloc(cap);
    const char* hit = strstr(src, anchor);
    size_t pre = (size_t)(hit - src);
    memcpy(out, src, pre);
    out[pre] = '\0';
    strcat(out, repl);
    strcat(out, hit + strlen(anchor));
    return out;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <angle|offset|winding> <input.h> <output.h>\n",
                argv[0]);
        return 2;
    }
    const char* mode = argv[1];
    size_t length;
    char* src = slurp(argv[2], &length);
    char* out = NULL;

    if (strcmp(mode, "angle") == 0) {
        /* Interior points of a spherical row: shrink the sweep angle. */
        out = replace_once(
            src,
            "paro_quat_rotate_vector(dst, quat, point_a);",
            "paro_quat_rotate_vector(dst, quat, point_a);\n"
            "        /* mutation angle: interior rotation rescaled */\n"
            "        if (point_index > 0) {\n"
            "            float mutated_quat[4];\n"
            "            paro_quat_from_rotation(mutated_quat, rotation_axis,\n"
            "                                     dtheta * point_index * 0.8f);\n"
            "            paro_quat_rotate_vector(dst, mutated_quat, point_a);\n"
            "        }",
            mode);
    } else if (strcmp(mode, "offset") == 0) {
        /* Drop the Z translation that moves patches onto the rounded box. */
        out = replace_once(
            src,
            "        xyz[2] += tz * sz;",
            "        xyz[2] += 0.0f; /* mutation offset: z patch translation removed */",
            mode);
    } else if (strcmp(mode, "winding") == 0) {
        /* Flip the first triangle (a, b, c) of every connector quad. */
        out = replace_once(
            src,
            "    *dst++ = a;\n    *dst++ = b;\n    *dst++ = c;\n    *dst++ = c;",
            "    *dst++ = a;\n    *dst++ = c;\n    *dst++ = b;\n    *dst++ = c;",
            mode);
    } else {
        fprintf(stderr, "mutate_header: unknown mode '%s'\n", mode);
        return 2;
    }

    FILE* f = fopen(argv[3], "wb");
    if (!f) {
        fprintf(stderr, "mutate_header: cannot write %s\n", argv[3]);
        return 2;
    }
    if (fwrite(out, 1, strlen(out), f) != strlen(out)) {
        fprintf(stderr, "mutate_header: short write on %s\n", argv[3]);
        return 2;
    }
    fclose(f);
    free(src);
    free(out);
    printf("mutate_header: wrote %s mutation to %s\n", mode, argv[3]);
    return 0;
}
