/*
prefix_trace example

Demonstrates distorm_decompose_prefix_trace(): the call mirrors
distorm_decompose(), but every decoded instruction gets a parallel
_PrefixTraceInfo describing what each prefix byte actually did.

Build (from the repository root), exactly like the other examples:
    cc -std=c99 -Iinclude -Isrc src/prefix_trace.c src/mnemonics.c src/textdefs.c \
       src/prefix.c src/operands.c src/insts.c src/instructions.c \
       src/distorm.c src/decoder.c examples/prefix_trace/main.c -o prefix_trace_demo
*/

#include <stdio.h>
#include <string.h>

#include "../../include/distorm.h"
#include "../../include/prefix_trace.h"

#define MAX_INSTRUCTIONS 16

static const char* group_name(uint8_t group)
{
	switch (group)
	{
		case PT_GROUP_OPERAND_SIZE: return "operand-size";
		case PT_GROUP_ADDRESS_SIZE: return "address-size";
		case PT_GROUP_SEGMENT: return "segment";
		case PT_GROUP_LOCK_REPEAT: return "lock/repeat";
		case PT_GROUP_REX: return "REX";
		case PT_GROUP_VEX: return "VEX";
		default: return "none";
	}
}

static void append_role(char* buf, unsigned bufSize, uint16_t roles)
{
	buf[0] = '\0';
	if (roles & PT_ROLE_OPERAND_SIZE) strncat(buf, "operand-size ", bufSize - 1);
	if (roles & PT_ROLE_ADDRESS_SIZE) strncat(buf, "address-size ", bufSize - 1);
	if (roles & PT_ROLE_SEGMENT) strncat(buf, "segment ", bufSize - 1);
	if (roles & PT_ROLE_LOCK_REPEAT) strncat(buf, "lock/repeat ", bufSize - 1);
	if (roles & PT_ROLE_MANDATORY_SELECTOR) strncat(buf, "mandatory-selector ", bufSize - 1);
	if (roles & PT_ROLE_REX) strncat(buf, "REX ", bufSize - 1);
	if (roles & PT_ROLE_VEX) strncat(buf, "VEX ", bufSize - 1);
	if (buf[0] == '\0') strcpy(buf, "(no effect)");
}

static const char* reason_name(uint8_t reason)
{
	switch (reason)
	{
		case PT_REASON_GROUP_OVERRIDE: return "overridden by a later byte of the same group";
		case PT_REASON_REX_POSITION: return "REX was not adjacent to the opcode";
		case PT_REASON_REX_W_OVER_66: return "REX.W forced 64-bit operand size over 0x66";
		case PT_REASON_MODE_IGNORED: return "ignored by the current decoding mode";
		case PT_REASON_INSTRUCTION_UNUSED: return "the instruction does not use this prefix";
		default: return "-";
	}
}

static void dump(const unsigned char* code, int codeLen, _DecodeType dt, const char* title)
{
	_CodeInfo ci;
	_DInst inst[MAX_INSTRUCTIONS];
	_PrefixTraceInfo trace[MAX_INSTRUCTIONS];
	unsigned used = 0, i, j;
	_DecodeResult res;

	printf("=== %s ===\n", title);

	memset(&ci, 0, sizeof(ci));
	ci.code = code;
	ci.codeLen = codeLen;
	ci.dt = dt;
	ci.codeOffset = 0x1000;

	/* Same arguments as distorm_decompose(), plus the parallel trace array. */
	res = distorm_decompose_prefix_trace(&ci, inst, trace, MAX_INSTRUCTIONS, &used);
	printf("result=%d used=%u nextOffset=0x%llx\n", res, used,
	       (unsigned long long)ci.nextOffset);

	for (i = 0; i < used; i++) {
		_DecodedInst text;
		char roleBuf[128];

		distorm_format(&ci, &inst[i], &text);
		printf("  [%u] %s %s (size=%u, unusedMask=0x%x, prefixRecords=%u)\n",
		       i, text.mnemonic.p, text.operands.p, inst[i].size,
		       inst[i].unusedPrefixesMask, trace[i].count);

		for (j = 0; j < trace[i].count; j++) {
			const _PrefixTrace* p = &trace[i].prefixes[j];
			unsigned k;

			append_role(roleBuf, sizeof(roleBuf), p->roles);
			printf("      +%u len=%u bytes=", p->offset, p->length);
			for (k = 0; k < p->byteCount; k++) printf("%02x ", p->bytes[k]);
			printf("group=%s roles={ %s} ", group_name(p->group), roleBuf);
			if (p->reason != PT_REASON_NONE) {
				printf("reason=\"%s\"", reason_name(p->reason));
				if (p->overriddenByOffset != 0xff) printf(" overriddenBy=+%u", p->overriddenByOffset);
			}
			if (p->group == PT_GROUP_VEX) printf(" impliedPp=%u", p->vexImpliedPp);
			printf("\n");
		}
	}
	printf("\n");
}

int main(void)
{
	/* Duplicate 0x66: first one is group-overridden, second one switches size. */
	const unsigned char dup66[] = {0x66, 0x66, 0xff, 0xc0};
	/* REX too far from the opcode: position failure; 0x66 is a mandatory selector. */
	const unsigned char rexPos[] = {0x48, 0x66, 0x0f, 0x10, 0xc0};
	/* F3 is a mandatory opcode selector here, not a string-repeat prefix. */
	const unsigned char mandatory[] = {0xf3, 0x0f, 0x10, 0xc0};
	/* Two-byte VEX with pp=1: one record, virtual mandatory selector reported. */
	const unsigned char vex[] = {0xc5, 0xf9, 0x10, 0xc0};

	dump(dup66, (int)sizeof(dup66), Decode64Bits, "duplicate operand-size prefixes");
	dump(rexPos, (int)sizeof(rexPos), Decode64Bits, "REX position failure");
	dump(mandatory, (int)sizeof(mandatory), Decode32Bits, "mandatory F3 selector");
	dump(vex, (int)sizeof(vex), Decode64Bits, "VEX prefix");

	return 0;
}
