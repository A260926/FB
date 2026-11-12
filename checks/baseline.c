#include <stdlib.h>
#include <math.h>
#include <assert.h>
#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"
#include "fixture.h"
#define RATE 44100
#define TICK 882
static unsigned char module[MOD_BYTES];
static float pcm[512][2];
static void frames(pocketmod_context *c, int count, float *last)
{
    while (count) {
        int want = count < 512 ? count : 512;
        int bytes = pocketmod_render(c, pcm, want * (int)sizeof(pcm[0]));
        int got = bytes / (int)sizeof(pcm[0]);
        assert(got > 0 && got <= want);
        if (last) *last = pcm[got - 1][0];
        count -= got;
    }
}
static void normal(const char *path)
{
    pocketmod_context a, b;
    float whole[997][2], split[997][2];
    int n = 0;
    mod_make(module);
    assert(mod_write(path, module));
    assert(pocketmod_init(&a, module, MOD_BYTES, RATE));
    assert(pocketmod_init(&b, module, MOD_BYTES, RATE));
    assert(pocketmod_render(&a, whole, sizeof(whole)) == sizeof(whole));
    while (n < 997) {
        int want = (n % 17) + 1;
        if (want > 997 - n) want = 997 - n;
        int got = pocketmod_render(&b, split + n, want * sizeof(split[0]));
        assert(got > 0); n += got / sizeof(split[0]);
    }
    assert(memcmp(whole, split, sizeof(whole)) == 0);
    assert(whole[0][0] > 0.07f && whole[0][1] > 0.04f);
    assert(pocketmod_loop_count(&a) == 0);
    printf("NORMAL_PCM_OK frames=997 first=%.9f\n", whole[0][0]);
}
static void witnesses(void)
{
    pocketmod_context c;
    float last;
    mod_make(module);
    mod_event(module, 0, 0, 1, 0, 0, 0xb, 1);
    mod_event(module, 0, 0, 2, 0, 0, 0xd, 2);
    assert(pocketmod_init(&c, module, MOD_BYTES, RATE));
    frames(&c, TICK * 6 + 1, &last);
    printf("WITNESS_BD current_order=%d row=%d pcm_left=%.9f (destination should be order=1,row=2,negative tone)\n", c.pattern, c.line, last);
    mod_make(module);
    mod_event(module, 0, 1, 0, 2, 428, 0xe, 0xd2);
    assert(pocketmod_init(&c, module, MOD_BYTES, RATE));
    frames(&c, TICK * 6 + 16, &last);
    printf("WITNESS_ED tick=%d pcm_left=%.9f sample=%d period=%d position=%.6f increment=%.6f real_volume=%d (old positive tone should continue until tick=2)\n", c.tick, last, c.channels[0].sample, c.channels[0].period, c.channels[0].position, c.channels[0].increment, c.channels[0].real_volume);
    mod_make(module);
    mod_event(module, 0, 0, 1, 0, 0, 0xe, 0x60);
    mod_event(module, 0, 2, 1, 0, 0, 0xe, 0x61);
    assert(pocketmod_init(&c, module, MOD_BYTES, RATE));
    frames(&c, TICK * 6 * 3, &last);
    printf("WITNESS_E6 loop_count=%d order=%d row=%d (local repeat should not end export)\n", pocketmod_loop_count(&c), c.pattern, c.line);
    mod_make(module);
    mod_event(module, 0, 1, 0, 0, 0, 0xe, 0xa1);
    mod_event(module, 0, 1, 1, 0, 0, 0xe, 0xe1);
    mod_event(module, 0, 0, 0, 1, 428, 0xc, 10);
    assert(pocketmod_init(&c, module, MOD_BYTES, RATE));
    frames(&c, TICK * 6 * 2, &last);
    printf("WITNESS_EE volume=%d row=%d tick=%d (fine slide should have run once, volume=11)\n", c.channels[0].volume, c.line, c.tick);
    puts("POCKETMOD_BASELINE_OK");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    normal(argv[1]);
    witnesses();
    return 0;
}
