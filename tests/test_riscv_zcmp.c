/* Capstone Disassembler Engine */
/* Example: decode RISC-V Zcmp 1.0 instructions (CS_MODE_RISCV_ZCMP).
 *
 * The byte array below is synthetic data.  It is only read, never executed.
 */

#include <stdio.h>

#include <capstone/capstone.h>

static void print_operand(csh handle, const cs_riscv_op *op)
{
	unsigned n, last;
	int first;

	switch (op->type) {
	case RISCV_OP_REG:
		printf("%s", cs_reg_name(handle, op->reg));
		break;
	case RISCV_OP_IMM:
		printf("%" PRId64, op->imm);
		break;
	case RISCV_OP_REGLIST:
		/* Bit n of the list stands for xn. */
		printf("{");
		first = 1;
		for (n = 1; n < 32; n++) {
			if (!RISCV_REGLIST_REG(op->reglist, n))
				continue;
			last = n;
			while (last + 1 < 32 &&
			       RISCV_REGLIST_REG(op->reglist, last + 1))
				last++;
			printf("%s%s", first ? "" : ", ",
			       cs_reg_name(handle, RISCV_REG_X0 + n));
			if (last > n) {
				printf("-%s",
				       cs_reg_name(handle, RISCV_REG_X0 + last));
				n = last;
			}
			first = 0;
		}
		printf("}");
		break;
	default:
		printf("?");
		break;
	}
}

int main(void)
{
	/* Synthetic encoding stream: cm.push, cm.popretz, cm.mva01s, addi. */
	static const unsigned char code[] = {
		0x62, 0xb8,	/* cm.push {ra, s0-s1}, -16      */
		0x62, 0xbc,	/* cm.popretz {ra, s0-s1}, 16    */
		0x66, 0xac,	/* cm.mva01s s0, s1              */
		0x13, 0x05, 0x10, 0x00	/* addi a0, zero, 1 */
	};
	csh handle;
	cs_insn *insn;
	size_t count, i;
	cs_regs regs_read, regs_write;
	uint8_t read_count, write_count;

	if (cs_open(CS_ARCH_RISCV, CS_MODE_RISCV32 | CS_MODE_RISCVC |
					CS_MODE_RISCV_ZCMP, &handle) != CS_ERR_OK) {
		printf("Failed to initialize Capstone\n");
		return 1;
	}

	if (cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
		printf("Failed to enable detail mode\n");
		cs_close(&handle);
		return 1;
	}

	count = cs_disasm(handle, code, sizeof(code), 0x1000, 0, &insn);
	if (!count) {
		printf("Failed to disassemble the given code\n");
		cs_close(&handle);
		return 1;
	}

	for (i = 0; i < count; i++) {
		unsigned j;

		printf("0x%" PRIx64 ":\t%s\t%s\n", insn[i].address,
		       insn[i].mnemonic, insn[i].op_str);

		for (j = 0; j < insn[i].detail->riscv.op_count; j++) {
			printf("\top%d: ", j);
			print_operand(handle, &insn[i].detail->riscv.operands[j]);
			printf("\n");
		}

		if (cs_regs_access(handle, &insn[i], regs_read, &read_count,
				   regs_write, &write_count) == CS_ERR_OK) {
			unsigned k;

			printf("\tread: ");
			for (k = 0; k < read_count; k++)
				printf("%s%s", k ? ", " : "",
				       cs_reg_name(handle, regs_read[k]));
			printf("\n\twrite: ");
			for (k = 0; k < write_count; k++)
				printf("%s%s", k ? ", " : "",
				       cs_reg_name(handle, regs_write[k]));
			printf("\n");
		}
	}

	cs_free(insn, count);
	cs_close(&handle);

	return 0;
}
