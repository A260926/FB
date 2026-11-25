#include "smk_export.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    char *end = NULL;
    unsigned long mask;
    int result;

    if (argc != 4) {
        fprintf(stderr, "usage: %s INPUT.SMK OUTPUT.AVI AUDIO_MASK_DECIMAL\n",
                argv[0] ? argv[0] : "smk-export");
        return 2;
    }

    mask = strtoul(argv[3], &end, 10);
    if (!end || *end != '\0' || mask > 0x7fu) {
        fprintf(stderr, "audio_mask must be a decimal integer from 0 to 127\n");
        return 2;
    }

    result = smk_export_avi(argv[1], argv[2], (unsigned)mask);
    if (result != 0) {
        fprintf(stderr, "SMK to AVI export failed\n");
        return 1;
    }
    return 0;
}
