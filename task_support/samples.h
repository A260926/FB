#ifndef TASK_SAMPLES_H
#define TASK_SAMPLES_H
#include <fcml_disassembler.h>
struct sample { const char *name; unsigned char bytes[15]; unsigned count; fcml_en_operating_mode mode; };
static const struct sample samples[] = {
 {"signed_imm",{0x83,0xc0,0xff},3,FCML_OM_32_BIT},
 {"rip_plus_imm",{0x48,0x83,0x05,0x11,0,0,0,0xff},8,FCML_OM_64_BIT},
 {"branch",{0xe8,1,0,0,0},5,FCML_OM_64_BIT},
 {"moffs",{0xa1,0x78,0x56,0x34,0x12},5,FCML_OM_32_BIT},
 {"far",{0xea,0x78,0x56,0x34,0x12,0x9a,0xbc},7,FCML_OM_32_BIT},
 {"sib",{0x83,0x44,0x88,0xf0,0x7f},5,FCML_OM_32_BIT}
};
#endif
