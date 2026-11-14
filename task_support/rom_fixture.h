#ifndef TASK_ROM_FIXTURE_H
#define TASK_ROM_FIXTURE_H
#include <stdint.h>
/* Hand-authored immutable data for reading only; no host execution or emulation. */
static const unsigned char task_rom_main[] = {
    0xe8,0x02,0x00,0x90,0xf4,0xc3,
    0xf4,0xf4,0xf4,0xf4,0xf4,0xf4,0xf4,0xf4,0xf4,0xf4,
    0x9a,0x10,0x00,0x00,0x20,0xf4
};
static const unsigned char task_rom_far[] = {0xcb};
#define TASK_ROM_MAIN_PHYSICAL UINT32_C(0x10000)
#define TASK_ROM_FAR_PHYSICAL UINT32_C(0x20010)
#define TASK_ROM_ENTRY_CS UINT16_C(0x1000)
#define TASK_ROM_ENTRY_IP UINT16_C(0x0000)
#define TASK_ROM_SECOND_IP UINT16_C(0x0010)
#endif
