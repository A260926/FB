/*
 * BeaEngine - private internals shared by the VSIB sources.
 *
 * The public DISASM Reserved_ field is the engine's thread local decode
 * state (called GV inside BeaEngine itself). It is an implementation
 * detail and is never exposed through the public vsib.h protocol.
 */

#ifndef BEA_VSIB_INTERNAL_H
#define BEA_VSIB_INTERNAL_H

#include <beaengine/vsib.h>

/* Mirror of the engine's GV macro, scoped to functions taking pMyDisasm. */
#define VSIB_GV(pMyDisasm) ((pMyDisasm)->Reserved_)

/* Raw map3 (0F 38) opcode bytes handled by this module. */
#define VSIB_OP_GATHER_DD_DQ  0x90
#define VSIB_OP_GATHER_QD_QQ  0x91
#define VSIB_OP_SCATTER_DD_DQ 0xA0
#define VSIB_OP_SCATTER_QD_QQ 0xA1

/* Maximum possible lane count (512 bit vector / 32 bit elements). */
#define VSIB_MAX_LANES BEA_VSIB_MAX_LANES

#endif /* BEA_VSIB_INTERNAL_H */
