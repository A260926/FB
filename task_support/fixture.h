#ifndef MICROMOD_TASK_FIXTURE_H
#define MICROMOD_TASK_FIXTURE_H
#include <string.h>
/* Public synthetic MOD material, generated here; no downloaded music. */
#define FIXTURE_BYTES (1084 + 1024 + 1024 + 64)
static void fixture_word(signed char *p, unsigned value) {
    p[0] = (signed char)(value >> 8); p[1] = (signed char)value;
}
static void fixture_note(signed char *m, int row, int chan,
                         int instrument, int period, int effect, int param) {
    signed char *p = m + 1084 + (row * 4 + chan) * 4;
    p[0] = (signed char)((instrument & 16) | (period >> 8));
    p[1] = (signed char)period;
    p[2] = (signed char)((instrument << 4) | effect);
    p[3] = (signed char)param;
}
static void fixture_mod(signed char *m, int kind) {
    int i;
    memset(m, 0, FIXTURE_BYTES);
    memcpy(m, "Public synthetic MOD", 20);
    m[950] = 1; memcpy(m + 1080, "M.K.", 4);
    fixture_word(m + 42, 512); m[45] = 64;
    fixture_word(m + 46, 64); fixture_word(m + 48, 64);
    fixture_word(m + 72, 32); m[75] = 64;
    fixture_word(m + 76, 8); fixture_word(m + 78, 16);
    for (i = 0; i < 1024; ++i) m[2108+i] = (signed char)(20 + i % 31);
    for (i = 0; i < 64; ++i) m[3132+i] = (signed char)(-20 - i % 23);
    fixture_note(m, 0, 0, 1, 428, 0, 0);
    fixture_note(m, 0, 3, 0, 0, 15, 1);
    if (kind == 1) fixture_note(m, 1, 0, 2, 0, 0, 0);
    if (kind == 2) fixture_note(m, 0, 0, 1, 428, 14, 255);
}
#endif
