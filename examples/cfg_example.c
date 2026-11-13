/* Minimal cs_cfg_recover_x86_64() client example.
 *
 * The image below contains a small function that computes an address through
 * a RIP relative table base and jumps through an absolute eight-byte jump
 * table. Everything is built in memory: the recovery engine never touches the
 * host address space and never executes the bytes.
 *
 * Build (CMake):
 *   cmake -S . -B build -DCAPSTONE_X86_SUPPORT=ON \
 *         -DCAPSTONE_BUILD_CFG_EXAMPLE=ON
 *   cmake --build build
 *   ./build/capstone_cfg_example
 */

#include <capstone/capstone.h>
#include <capstone/cfg.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define CODE_BASE 0x1000ULL
#define DATA_BASE 0x4000ULL

static void emit32(unsigned char **p, uint32_t v)
{
	*(*p)++ = (unsigned char)v;
	*(*p)++ = (unsigned char)(v >> 8);
	*(*p)++ = (unsigned char)(v >> 16);
	*(*p)++ = (unsigned char)(v >> 24);
}

static void emit64(unsigned char **p, uint64_t v)
{
	int i;
	for (i = 0; i < 8; ++i)
		*(*p)++ = (unsigned char)(v >> (8 * i));
}

int main(void)
{
	unsigned char code[128];
	unsigned char data[32];
	unsigned char *p = code;
	cs_cfg_region regions[2];
	cs_cfg_report *report = NULL;
	uint64_t entry = CODE_BASE;
	uint64_t target_a, target_b;
	size_t i;

	memset(code, 0, sizeof code);
	memset(data, 0, sizeof data);

	/* 0x1000: xor eax, eax                 ; index = 0 */
	*p++ = 0x31; *p++ = 0xc0;
	/* 0x1002: lea rbx, [rip + disp]        ; rbx = DATA_BASE */
	{
		uint64_t insn = CODE_BASE + (uint64_t)(p - code);
		int32_t rel = (int32_t)(DATA_BASE - (insn + 7));
		*p++ = 0x48; *p++ = 0x8d; *p++ = 0x1d;
		emit32(&p, (uint32_t)rel);
	}
	/* mov rax, [rbx + rax*8]               ; absolute table load */
	*p++ = 0x48; *p++ = 0x8b; *p++ = 0x04; *p++ = 0xc3;
	/* jmp rax */
	*p++ = 0xff; *p++ = 0xe0;

	/* two table targets, both ordinary code ending in ret */
	target_a = CODE_BASE + (uint64_t)(p - code);
	*p++ = 0x41; *p++ = 0xb8; *p++ = 0x07;
	*p++ = 0; *p++ = 0; *p++ = 0;
	*p++ = 0xc3;
	target_b = CODE_BASE + (uint64_t)(p - code);
	*p++ = 0x41; *p++ = 0xb9; *p++ = 0x09;
	*p++ = 0; *p++ = 0; *p++ = 0;
	*p++ = 0xc3;

	{
		unsigned char *q = data;
		emit64(&q, target_a);
		emit64(&q, target_b);
	}

	regions[0].address = CODE_BASE;
	regions[0].data = code;
	regions[0].size = (uint64_t)(p - code);
	regions[0].executable = true;
	regions[0].read_only = false;

	regions[1].address = DATA_BASE;
	regions[1].data = data;
	regions[1].size = sizeof data;
	regions[1].executable = false;
	regions[1].read_only = true;

	if (cs_cfg_recover_x86_64(regions, 2, &entry, 1, &report) !=
			CS_ERR_OK) {
		fputs("recovery failed\n", stderr);
		return 1;
	}

	printf("recovered %zu blocks, %zu edges, %zu indirect targets\n",
			report->block_count, report->edge_count,
			report->indirect_target_count);
	for (i = 0; i < report->block_count; ++i) {
		printf(" block 0x%" PRIx64 " .. 0x%" PRIx64
			" terminator=%d instructions=%u\n",
			report->blocks[i].address,
			report->blocks[i].end,
			report->blocks[i].terminator,
			report->blocks[i].instruction_count);
	}
	for (i = 0; i < report->edge_count; ++i) {
		printf(" edge 0x%" PRIx64 " -> 0x%" PRIx64 " kind=%d\n",
			report->edges[i].source,
			report->edges[i].target,
			report->edges[i].kind);
	}
	for (i = 0; i < report->indirect_target_count; ++i) {
		printf(" indirect at 0x%" PRIx64 " -> 0x%" PRIx64 "\n",
			report->indirect_targets[i].site,
			report->indirect_targets[i].target);
	}
	printf("incomplete=%d diagnostics=%zu\n",
			report->incomplete, report->diagnostic_count);

	cs_cfg_report_free(report);
	return 0;
}
