/* Capstone Disassembler Engine */
/* Regression tests for the RISC-V Zcmp 1.0 extension (CS_MODE_RISCV_ZCMP). */

#include <stdio.h>
#include <string.h>

#include <capstone/capstone.h>
#include "task_support/check.h"

static csh open_riscv(cs_mode mode)
{
	csh handle = 0;

	if (cs_open(CS_ARCH_RISCV, mode, &handle) != CS_ERR_OK)
		return 0;
	if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
		cs_close(&handle);
		return 0;
	}

	return handle;
}

#define ZCMP32 (CS_MODE_RISCV32 | CS_MODE_RISCVC | CS_MODE_RISCV_ZCMP)
#define ZCMP64 (CS_MODE_RISCV64 | CS_MODE_RISCVC | CS_MODE_RISCV_ZCMP)

/* Format an instruction list into a stable string for comparison. */
static void format_insn(csh handle, const cs_insn *insn, char *buf, size_t len)
{
	snprintf(buf, len, "%s %s", insn->mnemonic, insn->op_str);
	(void)handle;
}

/* Disassemble one 16-bit value.  Returns 1 on success. */
static int disasm_one(csh handle, unsigned value, cs_insn **out, size_t *count)
{
	uint8_t bytes[2];

	bytes[0] = (uint8_t)(value & 0xff);
	bytes[1] = (uint8_t)((value >> 8) & 0xff);
	*count = cs_disasm(handle, bytes, sizeof(bytes), 0x1000, 0, out);

	return *count == 1;
}

static int check_insn_text(csh handle, unsigned value, const char *want)
{
	cs_insn *insn = NULL;
	size_t count;
	char buf[128];
	int ok;

	if (!disasm_one(handle, value, &insn, &count))
		return 0;

	format_insn(handle, insn, buf, sizeof(buf));
	ok = strcmp(buf, want) == 0;
	cs_free(insn, count);

	return ok;
}

/* ---------------------------------------------------------------- decode */

static int test_decode_push_pop_forms(void)
{
	csh handle = open_riscv(ZCMP32);
	int ok;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	/* {ra} and {ra, s0-s11}; note the jump from s0-s9 to s0-s11. */
	ok = check_insn_text(handle, 0xb842, "cm.push {ra}, -16") &&
	     check_insn_text(handle, 0xb8f2, "cm.push {ra, s0-s11}, -64") &&
	     check_insn_text(handle, 0xba62, "cm.pop {ra, s0-s1}, 16") &&
	     check_insn_text(handle, 0xbaf2, "cm.pop {ra, s0-s11}, 64") &&
	     check_insn_text(handle, 0xbc42, "cm.popretz {ra}, 16") &&
	     check_insn_text(handle, 0xbe42, "cm.popret {ra}, 16");

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_decode_mv_forms(void)
{
	csh handle = open_riscv(ZCMP32);
	int ok;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	ok = check_insn_text(handle, 0xac66, "cm.mva01s s0, s1") &&
	     check_insn_text(handle, 0xac26, "cm.mvsa01 s0, s1") &&
	     check_insn_text(handle, 0xacfe, "cm.mva01s s1, s7") &&
	     check_insn_text(handle, 0xac3e, "cm.mvsa01 s0, s7");

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_decode_xlen_adjustments(void)
{
	csh h32 = open_riscv(ZCMP32);
	csh h64 = open_riscv(ZCMP64);
	int ok;

	if (!h32 || !h64) {
		if (h32)
			cs_close(&h32);
		if (h64)
			cs_close(&h64);
		TASK_CHECK(0);
		return 1;
	}

	/* Same encoding, different XLEN: 2 registers x 8 bytes rounds to 32. */
	ok = check_insn_text(h32, 0xb862, "cm.push {ra, s0-s1}, -16") &&
	     check_insn_text(h64, 0xb862, "cm.push {ra, s0-s1}, -32") &&
	     check_insn_text(h32, 0xb8f2, "cm.push {ra, s0-s11}, -64") &&
	     check_insn_text(h64, 0xb8f2, "cm.push {ra, s0-s11}, -112");

	cs_close(&h32);
	cs_close(&h64);
	TASK_CHECK(ok);
	return 0;
}

static int test_decode_reserved_and_legacy(void)
{
	csh zmode = open_riscv(ZCMP32);
	csh legacy = open_riscv(CS_MODE_RISCV32 | CS_MODE_RISCVC);
	int ok = 1;

	if (!zmode || !legacy) {
		if (zmode)
			cs_close(&zmode);
		if (legacy)
			cs_close(&legacy);
		TASK_CHECK(0);
		return 1;
	}

	/* rlist 0..3 are reserved; the legacy table still sees c.fsdsp. */
	ok = check_insn_text(zmode, 0xb842, "cm.push {ra}, -16") &&
	     check_insn_text(legacy, 0xb842, "c.fsdsp fa6, 0x30(sp)");
	/* cm.mva01s/cm.mvsa01 overlap with c.fsdsp as well. */
	ok = ok && check_insn_text(zmode, 0xac66, "cm.mva01s s0, s1") &&
	     check_insn_text(legacy, 0xac66, "c.fsdsp fs9, 0x18(sp)");

	cs_close(&zmode);
	cs_close(&legacy);
	TASK_CHECK(ok);
	return 0;
}

static int test_decode_iter_matches_batch(void)
{
	static const uint8_t code[] = {
		0x42, 0xb8, 0xf2, 0xb8, 0x62, 0xba, 0x62, 0xbc,
		0x62, 0xbe, 0x66, 0xac, 0x26, 0xac
	};
	csh handle = open_riscv(ZCMP32);
	cs_insn *batch = NULL, *iter;
	const uint8_t *p;
	size_t count, size, i = 0;
	uint64_t address;
	char a[128], b[128];
	int ok = 1;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	count = cs_disasm(handle, code, sizeof(code), 0x2000, 0, &batch);
	iter = cs_malloc(handle);
	if (!count || !iter) {
		cs_free(batch, count);
		cs_free(iter, 1);
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}

	p = code;
	size = sizeof(code);
	address = 0x2000;
	while (cs_disasm_iter(handle, &p, &size, &address, iter)) {
		if (i >= count) {
			ok = 0;
			break;
		}
		format_insn(handle, &batch[i], a, sizeof(a));
		format_insn(handle, iter, b, sizeof(b));
		if (strcmp(a, b) || batch[i].id != iter->id ||
		    batch[i].size != iter->size ||
		    batch[i].detail->riscv.op_count !=
			    iter->detail->riscv.op_count)
			ok = 0;
		++i;
	}

	ok = ok && i == count;

	cs_free(batch, count);
	cs_free(iter, 1);
	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

/* --------------------------------------------------------------- details */

static int has_reg(const cs_regs regs, uint8_t count, unsigned reg)
{
	uint8_t i;

	for (i = 0; i < count; i++)
		if (regs[i] == reg)
			return 1;

	return 0;
}

static const cs_riscv_op *find_op(const cs_insn *insn, riscv_op_type type)
{
	unsigned i;

	for (i = 0; i < insn->detail->riscv.op_count; i++)
		if (insn->detail->riscv.operands[i].type == type)
			return &insn->detail->riscv.operands[i];

	return NULL;
}

static int test_details_reglist_operands(void)
{
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	const cs_riscv_op *list, *imm;
	uint32_t want = (1u << 1) | (1u << 8) | (1u << 9) |
			(1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) |
			(1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) |
			(1u << 26) | (1u << 27);
	int ok = 1;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	if (!disasm_one(handle, 0xb8f2, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}

	/* The list is not truncated into the eight legacy operand slots. */
	ok = ok && insn->detail->riscv.op_count == 2;
	ok = ok && insn->id == RISCV_INS_CM_PUSH;
	list = find_op(insn, RISCV_OP_REGLIST);
	imm = find_op(insn, RISCV_OP_IMM);
	ok = ok && list != NULL && list->reglist == want;
	ok = ok && imm != NULL && imm->imm == -64;
	/* Every saved register has its true identity, not an FPR. */
	ok = ok && (list->reglist & (1u << 27)) != 0;
	ok = ok && (list->reglist & (1u << 26)) != 0;
	ok = ok && (list->reglist & (1u << 16)) == 0;

	cs_free(insn, count);

	/* The maximum list is {ra, s0-s11}; there is no {ra, s0-s10} form, so
	 * rlist 14 still denotes s0-s9 while s10 always comes with s11. */
	if (!disasm_one(handle, 0xbaf2, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	list = find_op(insn, RISCV_OP_REGLIST);
	ok = ok && list != NULL && (list->reglist & (1u << 26)) != 0;
	ok = ok && (list->reglist & (1u << 27)) != 0;
	cs_free(insn, count);

	if (!disasm_one(handle, 0xbae2, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	list = find_op(insn, RISCV_OP_REGLIST);
	ok = ok && list != NULL && (list->reglist & (1u << 26)) == 0 &&
	     (list->reglist & (1u << 27)) == 0;
	cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_details_mv_operands(void)
{
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	const cs_riscv *r;
	int ok = 1;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	if (!disasm_one(handle, 0xac66, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}

	/* cm.mva01s: a0, a1 destinations then s0, s1 sources. */
	r = &insn->detail->riscv;
	ok = ok && r->op_count == 4;
	ok = ok && r->operands[0].type == RISCV_OP_REG &&
	     r->operands[0].reg == RISCV_REG_A0;
	ok = ok && r->operands[1].type == RISCV_OP_REG &&
	     r->operands[1].reg == RISCV_REG_A1;
	ok = ok && r->operands[2].type == RISCV_OP_REG &&
	     r->operands[2].reg == RISCV_REG_S0;
	ok = ok && r->operands[3].type == RISCV_OP_REG &&
	     r->operands[3].reg == RISCV_REG_S1;
	cs_free(insn, count);

	if (!disasm_one(handle, 0xac26, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	/* cm.mvsa01: s0, s1 destinations then a0, a1 sources. */
	r = &insn->detail->riscv;
	ok = ok && r->op_count == 4;
	ok = ok && r->operands[0].reg == RISCV_REG_S0;
	ok = ok && r->operands[1].reg == RISCV_REG_S1;
	ok = ok && r->operands[2].reg == RISCV_REG_A0;
	ok = ok && r->operands[3].reg == RISCV_REG_A1;
	cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_details_regs_access(void)
{
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	cs_regs rd, wr;
	uint8_t rc, wc;
	int ok = 1;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	/* cm.push: sp read/written, listed registers read. */
	if (!disasm_one(handle, 0xb862, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ok = ok && cs_regs_access(handle, insn, rd, &rc, wr, &wc) == CS_ERR_OK;
	ok = ok && has_reg(rd, rc, RISCV_REG_SP) &&
	     has_reg(wr, wc, RISCV_REG_SP) &&
	     has_reg(rd, rc, RISCV_REG_RA) &&
	     has_reg(rd, rc, RISCV_REG_S0) &&
	     has_reg(rd, rc, RISCV_REG_S1) &&
	     !has_reg(wr, wc, RISCV_REG_RA);
	cs_free(insn, count);

	/* cm.pop: listed registers written. */
	if (!disasm_one(handle, 0xba62, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ok = ok && cs_regs_access(handle, insn, rd, &rc, wr, &wc) == CS_ERR_OK;
	ok = ok && has_reg(rd, rc, RISCV_REG_SP) &&
	     has_reg(wr, wc, RISCV_REG_SP) &&
	     has_reg(wr, wc, RISCV_REG_RA) &&
	     has_reg(wr, wc, RISCV_REG_S1);
	cs_free(insn, count);

	/* cm.popret: ra is read as the return target. */
	if (!disasm_one(handle, 0xbe42, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ok = ok && cs_regs_access(handle, insn, rd, &rc, wr, &wc) == CS_ERR_OK;
	ok = ok && has_reg(rd, rc, RISCV_REG_RA) &&
	     has_reg(wr, wc, RISCV_REG_RA);
	cs_free(insn, count);

	/* cm.popretz: additionally zeroes a0. */
	if (!disasm_one(handle, 0xbc42, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ok = ok && cs_regs_access(handle, insn, rd, &rc, wr, &wc) == CS_ERR_OK;
	ok = ok && has_reg(rd, rc, RISCV_REG_RA) &&
	     has_reg(wr, wc, RISCV_REG_A0);
	cs_free(insn, count);

	/* cm.mva01s s0, s0: sources read, destinations written, de-duplicated. */
	if (!disasm_one(handle, 0xac62, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ok = ok && cs_regs_access(handle, insn, rd, &rc, wr, &wc) == CS_ERR_OK;
	ok = ok && has_reg(rd, rc, RISCV_REG_S0) &&
	     has_reg(wr, wc, RISCV_REG_A0) &&
	     has_reg(wr, wc, RISCV_REG_A1) &&
	     !has_reg(wr, wc, RISCV_REG_S0) && rc == 1;
	cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_details_no_stale_state(void)
{
	/* A reserved encoding must not inherit the previous list or access set. */
	static const uint8_t code[] = { 0xf2, 0xb8, 0x02, 0xb8 };
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	cs_regs rd, wr;
	uint8_t rc, wc;
	unsigned i;
	int ok, has_list = 0;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	/* Decode the real Zcmp push followed by the reserved 0x02b8. */
	count = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
	ok = count == 2 && insn[0].id == RISCV_INS_CM_PUSH;
	ok = ok && insn[1].id != RISCV_INS_CM_PUSH &&
	     insn[1].detail->riscv.op_count > 0;

	/* No stale register list survives into the legacy instruction. */
	for (i = 0; ok && i < insn[1].detail->riscv.op_count; i++)
		if (insn[1].detail->riscv.operands[i].type == RISCV_OP_REGLIST)
			has_list = 1;
	ok = ok && !has_list;

	if (ok) {
		ok = cs_regs_access(handle, &insn[1], rd, &rc, wr, &wc) ==
			     CS_ERR_OK &&
		     !has_reg(rd, rc, RISCV_REG_S11) &&
		     !has_reg(wr, wc, RISCV_REG_S11);
	}
	cs_free(insn, count);

	/* Truncated input reports no instruction at all. */
	insn = NULL;
	count = cs_disasm(handle, code, 1, 0x1000, 0, &insn);
	ok = ok && count == 0;
	if (count)
		cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

/* ---------------------------------------------------------------- compat */

static int test_compat_names_and_ids(void)
{
	csh handle = open_riscv(ZCMP32);
	int ok;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	ok = strcmp(cs_insn_name(handle, RISCV_INS_CM_PUSH), "cm.push") == 0 &&
	     strcmp(cs_insn_name(handle, RISCV_INS_CM_POP), "cm.pop") == 0 &&
	     strcmp(cs_insn_name(handle, RISCV_INS_CM_POPRET), "cm.popret") == 0 &&
	     strcmp(cs_insn_name(handle, RISCV_INS_CM_POPRETZ), "cm.popretz") == 0 &&
	     strcmp(cs_insn_name(handle, RISCV_INS_CM_MVA01S), "cm.mva01s") == 0 &&
	     strcmp(cs_insn_name(handle, RISCV_INS_CM_MVSA01), "cm.mvsa01") == 0;
	/* Existing enum values keep their meaning and numbering. */
	ok = ok && RISCV_INS_ADD == 1 &&
	     RISCV_INS_XORI == RISCV_INS_CM_PUSH - 1 &&
	     RISCV_INS_ENDING > RISCV_INS_CM_MVSA01;

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_compat_groups(void)
{
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	uint8_t i;
	int ok = 1, ret;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	/* popret and popretz belong to the RET group. */
	if (!disasm_one(handle, 0xbc42, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ret = 0;
	for (i = 0; i < insn->detail->groups_count; i++)
		if (insn->detail->groups[i] == RISCV_GRP_RET)
			ret = 1;
	ok = ok && ret && insn->id == RISCV_INS_CM_POPRETZ;
	cs_free(insn, count);

	if (!disasm_one(handle, 0xbe42, &insn, &count)) {
		cs_close(&handle);
		TASK_CHECK(0);
		return 1;
	}
	ret = 0;
	for (i = 0; i < insn->detail->groups_count; i++)
		if (insn->detail->groups[i] == RISCV_GRP_RET)
			ret = 1;
	ok = ok && ret && insn->id == RISCV_INS_CM_POPRET;
	cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_compat_mode_switch(void)
{
	static const uint8_t code[] = { 0x66, 0xac };
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	int ok;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	count = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
	ok = count == 1 && insn->id == RISCV_INS_CM_MVA01S;
	cs_free(insn, count);

	/* Turning the extension off at run time drops back to legacy. */
	ok = ok && cs_option(handle, CS_OPT_MODE,
			     CS_MODE_RISCV64 | CS_MODE_RISCVC) == CS_ERR_OK;
	insn = NULL;
	count = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
	ok = ok && count == 1 && insn->id == RISCV_INS_C_FSDSP;
	cs_free(insn, count);

	/* And re-enabling it restores Zcmp decoding. */
	ok = ok && cs_option(handle, CS_OPT_MODE, ZCMP64) == CS_ERR_OK;
	insn = NULL;
	count = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
	ok = ok && count == 1 && insn->id == RISCV_INS_CM_MVA01S &&
	     insn->detail->riscv.op_count == 4;
	cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

static int test_compat_existing_detail(void)
{
	/* A non-Zcmp instruction keeps the legacy operand encoding. */
	csh handle = open_riscv(ZCMP32);
	cs_insn *insn = NULL;
	size_t count;
	int ok = 0;

	if (!handle) {
		TASK_CHECK(0);
		return 1;
	}

	insn = NULL;
	count = cs_disasm(handle, (const uint8_t *)"\x13\x05\x10\x00", 4,
			  0x1000, 0, &insn);
	if (count == 1) {
		const cs_riscv *r = &insn->detail->riscv;

		ok = insn->id == RISCV_INS_ADDI && r->op_count == 3 &&
		     r->operands[0].type == RISCV_OP_REG &&
		     r->operands[0].reg == RISCV_REG_A0 &&
		     r->operands[1].type == RISCV_OP_REG &&
		     r->operands[1].reg == RISCV_REG_ZERO &&
		     r->operands[2].type == RISCV_OP_IMM &&
		     r->operands[2].imm == 1;
	}
	cs_free(insn, count);

	cs_close(&handle);
	TASK_CHECK(ok);
	return 0;
}

const struct task_case task_cases[] = {
	{ "decode", "push_pop_forms", test_decode_push_pop_forms },
	{ "decode", "mv_forms", test_decode_mv_forms },
	{ "decode", "xlen_adjustments", test_decode_xlen_adjustments },
	{ "decode", "reserved_and_legacy", test_decode_reserved_and_legacy },
	{ "decode", "iter_matches_batch", test_decode_iter_matches_batch },
	{ "details", "reglist_operands", test_details_reglist_operands },
	{ "details", "mv_operands", test_details_mv_operands },
	{ "details", "regs_access", test_details_regs_access },
	{ "details", "no_stale_state", test_details_no_stale_state },
	{ "compat", "names_and_ids", test_compat_names_and_ids },
	{ "compat", "groups", test_compat_groups },
	{ "compat", "mode_switch", test_compat_mode_switch },
	{ "compat", "existing_detail", test_compat_existing_detail },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
