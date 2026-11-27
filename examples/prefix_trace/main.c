/*
prefix_trace example

Demonstrates distorm_decompose_prefix_trace: decode a buffer and print, for
every instruction, what each prefix byte ended up doing - or why it had no
effect at all.

Build (from the repository root):
	cc -std=c99 -Iinclude -Isrc src/distorm.c src/decoder.c src/instructions.c \
	   src/insts.c src/mnemonics.c src/operands.c src/prefix.c src/prefix_trace.c \
	   src/textdefs.c examples/prefix_trace/main.c -o prefix_trace
*/

#include <stdio.h>
#include <string.h>

#include "../../include/distorm.h"
#include "../../include/prefix_trace.h"

static const char* group_names[] = {
	"none", "operand-size", "address-size", "segment", "lock/repeat", "rex", "vex"
};

static const char* reason_names[] = {
	"-", "superseded", "rex-position", "rex.w-over-opsize", "mode-ignored", "not-used"
};

static void print_roles(unsigned int roles)
{
	static const struct { unsigned int bit; const char* name; } role_names[] = {
		{ PFX_ROLE_OPERAND_SIZE, "operand-size" },
		{ PFX_ROLE_ADDRESS_SIZE, "address-size" },
		{ PFX_ROLE_SEGMENT, "segment" },
		{ PFX_ROLE_LOCK, "lock" },
		{ PFX_ROLE_REPEAT, "repeat" },
		{ PFX_ROLE_MANDATORY, "mandatory" },
		{ PFX_ROLE_REX, "rex" },
		{ PFX_ROLE_VEX, "vex" },
	};
	unsigned int i;
	int first = 1;
	if (roles == PFX_ROLE_NONE) {
		printf("(none)");
		return;
	}
	for (i = 0; i < sizeof(role_names) / sizeof(role_names[0]); i++) {
		if (roles & role_names[i].bit) {
			printf("%s%s", first ? "" : "|", role_names[i].name);
			first = 0;
		}
	}
}

int main(void)
{
	/* 66 48 98       - CDQE, the 0x66 loses to REX.W
	 * 66 66 90       - NOP, one 0x66 overrides the other
	 * 48 66 0f 10 c0 - MOVUPD, the REX sits too far from the opcode
	 * f3 0f 10 c0    - MOVSS, f3 is a mandatory opcode selector
	 * c5 f9 6f c1    - VMOVDQA, VEX pp implies a 0x66 selector
	 */
	static const uint8_t code[] = {
		0x66, 0x48, 0x98,
		0x66, 0x66, 0x90,
		0x48, 0x66, 0x0f, 0x10, 0xc0,
		0xf3, 0x0f, 0x10, 0xc0,
		0xc5, 0xf9, 0x6f, 0xc1
	};

	_CodeInfo ci;
	_DInst insts[16];
	_PrefixTraceInfo traces[16];
	unsigned int used = 0, i, j;
	_OffsetType offset;

	memset(&ci, 0, sizeof(ci));
	ci.code = code;
	ci.codeLen = sizeof(code);
	ci.dt = Decode64Bits;

	if (distorm_decompose_prefix_trace(&ci, insts, traces, 16, &used) != DECRES_SUCCESS) {
		printf("decoding failed\n");
		return 1;
	}

	for (i = 0; i < used; i++) {
		offset = insts[i].addr;
		printf("%06llx: ", (unsigned long long)offset);
		for (j = 0; j < insts[i].size; j++) printf("%02x", code[offset + j]);
		printf(" (unused mask 0x%x)\n", insts[i].unusedPrefixesMask);

		for (j = 0; j < traces[i].count; j++) {
			const _PrefixTrace* t = &traces[i].items[j];
			printf("  +%u %02x %-12s roles=", t->offset, t->byte, group_names[t->group]);
			print_roles(t->roles);
			if (t->reason != PFX_REASON_NONE) {
				printf(" reason=%s", reason_names[t->reason]);
				if (t->supersededBy != PFX_TRACE_NO_OFFSET) printf(" by=+%u", t->supersededBy);
			}
			if (t->selector) printf(" selector=%02x", t->selector);
			printf("\n");
		}
	}
	return 0;
}
