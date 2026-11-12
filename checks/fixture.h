/* Original, generated MOD fixtures. MIT licensed. */
#ifndef CHECK_FIXTURE_H
#define CHECK_FIXTURE_H
#include <string.h>
#include <stdio.h>
#define MOD_PATTERNS 3
#define MOD_SAMPLE_BYTES 8192
#define MOD_BYTES (1084 + MOD_PATTERNS * 1024 + 2 * MOD_SAMPLE_BYTES + 64)
static void mod_event(unsigned char *m, int pat, int row, int ch,
                      int sample, int period, int fx, int param)
{
    unsigned char *p = m + 1084 + ((pat * 64 + row) * 4 + ch) * 4;
    p[0] = (unsigned char)((sample & 0xf0) | (period >> 8));
    p[1] = (unsigned char)period;
    p[2] = (unsigned char)((sample << 4) | fx);
    p[3] = (unsigned char)param;
}
static void mod_make(unsigned char *m)
{
    int i;
    memset(m, 0, MOD_BYTES);
    memcpy(m, "sample local tones", 18);
    memcpy(m + 1080, "M.K.", 4);
    m[950] = 3;
    m[952] = 0; m[953] = 1; m[954] = 2;
    for (i = 0; i < 2; ++i) {
        unsigned char *s = m + 20 + i * 30;
        s[22] = 16; s[25] = 64; s[28] = 16;
        memset(m + 1084 + MOD_PATTERNS * 1024 + MOD_SAMPLE_BYTES * i,
               i == 0 ? 64 : 192, MOD_SAMPLE_BYTES);
    }
    mod_event(m, 0, 0, 0, 1, 428, 0, 0);
    mod_event(m, 1, 2, 0, 2, 428, 0, 0);
    mod_event(m, 2, 2, 0, 1, 428, 0xc, 16);
}
static int mod_write(const char *path, const unsigned char *m)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(m, 1, MOD_BYTES, f) == MOD_BYTES;
    return fclose(f) == 0 && ok;
}
#endif
