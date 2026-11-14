#include "task_support/headless.h"
int palette_search(const palette_t *palette, const uint8_t col[4],
                   bool exact)
{
    int i;
    assert(exact); // For the moment.
    for (i = 0; i < palette->size; i++) {
        if (memcmp(col, palette->entries[i].color, 4) == 0)
            return i;
    }
    return -1;
}
