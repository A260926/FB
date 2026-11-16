/*
 * Regression tests for the APX `REX2` (`D5`) decoding support.
 *
 * The instruction bytes below are pure input data. They are never executed and no elevated
 * state, APX-capable machine or readable memory is required.
 */

#include "../task_support/check.h"
#include <Zydis/Zydis.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

/* ---------------------------------------------------------------------------------------------- */
/* Helpers                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

static ZyanStatus DecodeFull64(const ZyanU8* buffer, ZyanUSize length,
    ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operands)
{
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_STACK_WIDTH_64)))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    return ZydisDecoderDecodeFull(&decoder, buffer, length, instruction, operands);
}

static ZyanStatus DecodeSplit64(const ZyanU8* buffer, ZyanUSize length,
    ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operands)
{
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_STACK_WIDTH_64)))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    ZydisDecoderContext context;
    ZyanStatus status = ZydisDecoderDecodeInstruction(&decoder, &context, buffer, length,
        instruction);
    if (!ZYAN_SUCCESS(status))
    {
        return status;
    }
    return ZydisDecoderDecodeOperands(&decoder, &context, instruction, operands,
        instruction->operand_count);
}

static ZyanStatus Format(ZydisFormatterStyle style, const ZydisDecodedInstruction* instruction,
    const ZydisDecodedOperand* operands, ZyanU64 runtime_address, char* buffer, ZyanUSize length)
{
    ZydisFormatter formatter;
    if (!ZYAN_SUCCESS(ZydisFormatterInit(&formatter, style)))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    return ZydisFormatterFormatInstruction(&formatter, instruction, operands,
        instruction->operand_count_visible, buffer, length, runtime_address, ZYAN_NULL);
}

/*
 * Compares the visible state produced by `ZydisDecoderDecodeFull` and the two-step
 * `ZydisDecoderDecodeInstruction`/`ZydisDecoderDecodeOperands` sequence. Returns 0 if both agree.
 */
static int CompareDecodeEntries(const ZyanU8* buffer, ZyanUSize length,
    const ZydisDecodedInstruction* full, const ZydisDecodedOperand* full_ops,
    const ZydisDecodedInstruction* split, const ZydisDecodedOperand* split_ops)
{
    TASK_CHECK(full->length == split->length);
    TASK_CHECK(full->mnemonic == split->mnemonic);
    TASK_CHECK(full->operand_count == split->operand_count);
    TASK_CHECK(full->operand_count_visible == split->operand_count_visible);
    TASK_CHECK(full->operand_width == split->operand_width);
    TASK_CHECK(full->address_width == split->address_width);
    TASK_CHECK(full->attributes == split->attributes);
    TASK_CHECK(full->raw.rex2.offset == split->raw.rex2.offset);
    TASK_CHECK(full->raw.rex2.payload == split->raw.rex2.payload);
    TASK_CHECK(full->raw.modrm.offset == split->raw.modrm.offset);
    TASK_CHECK(full->raw.sib.offset == split->raw.sib.offset);
    TASK_CHECK(full->raw.disp.offset == split->raw.disp.offset);
    TASK_CHECK(full->raw.disp.size == split->raw.disp.size);
    TASK_CHECK(full->raw.imm[0].offset == split->raw.imm[0].offset);
    TASK_CHECK(full->raw.imm[0].size == split->raw.imm[0].size);
    TASK_CHECK(full->raw.imm[0].value.u == split->raw.imm[0].value.u);
    for (ZyanU8 i = 0; i < full->operand_count; ++i)
    {
        TASK_CHECK(full_ops[i].type == split_ops[i].type);
        TASK_CHECK(full_ops[i].visibility == split_ops[i].visibility);
        TASK_CHECK(full_ops[i].actions == split_ops[i].actions);
        TASK_CHECK(full_ops[i].encoding == split_ops[i].encoding);
        TASK_CHECK(full_ops[i].size == split_ops[i].size);
        switch (full_ops[i].type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            TASK_CHECK(full_ops[i].reg.value == split_ops[i].reg.value);
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            TASK_CHECK(full_ops[i].mem.base == split_ops[i].mem.base);
            TASK_CHECK(full_ops[i].mem.index == split_ops[i].mem.index);
            TASK_CHECK(full_ops[i].mem.scale == split_ops[i].mem.scale);
            TASK_CHECK(full_ops[i].mem.disp.has_displacement ==
                split_ops[i].mem.disp.has_displacement);
            TASK_CHECK(full_ops[i].mem.disp.value == split_ops[i].mem.disp.value);
            TASK_CHECK(full_ops[i].mem.segment == split_ops[i].mem.segment);
            break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            TASK_CHECK(full_ops[i].imm.value.u == split_ops[i].imm.value.u);
            TASK_CHECK(full_ops[i].imm.is_signed == split_ops[i].imm.is_signed);
            TASK_CHECK(full_ops[i].imm.is_relative == split_ops[i].imm.is_relative);
            break;
        default:
            break;
        }
    }
    (void)buffer;
    (void)length;
    return 0;
}

/* Decodes with both entry points and checks that they agree. */
#define CHECK_BOTH(buffer, length, instruction, operands) \
    do \
    { \
        ZydisDecodedInstruction split_instruction_; \
        ZydisDecodedOperand split_operands_[ZYDIS_MAX_OPERAND_COUNT]; \
        TASK_CHECK(ZYAN_SUCCESS(DecodeFull64((buffer), (length), (instruction), (operands)))); \
        TASK_CHECK(ZYAN_SUCCESS(DecodeSplit64((buffer), (length), &split_instruction_, \
            split_operands_))); \
        if (CompareDecodeEntries((buffer), (length), (instruction), (operands), \
            &split_instruction_, split_operands_)) \
        { \
            return 1; \
        } \
    } while (0)

/* Checks the textual output in both formatter styles. */
#define CHECK_TEXT(buffer, length, runtime_address, expected_intel, expected_att) \
    do \
    { \
        ZydisDecodedInstruction instruction_; \
        ZydisDecodedOperand operands_[ZYDIS_MAX_OPERAND_COUNT]; \
        TASK_CHECK(ZYAN_SUCCESS(DecodeFull64((buffer), (length), &instruction_, operands_))); \
        char intel_[256]; \
        char att_[256]; \
        TASK_CHECK(ZYAN_SUCCESS(Format(ZYDIS_FORMATTER_STYLE_INTEL, &instruction_, operands_, \
            (runtime_address), intel_, sizeof(intel_)))); \
        TASK_CHECK(ZYAN_SUCCESS(Format(ZYDIS_FORMATTER_STYLE_ATT, &instruction_, operands_, \
            (runtime_address), att_, sizeof(att_)))); \
        TASK_CHECK(strcmp(intel_, (expected_intel)) == 0); \
        TASK_CHECK(strcmp(att_, (expected_att)) == 0); \
    } while (0)

/* ---------------------------------------------------------------------------------------------- */
/* Prefixes                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

static int TestRex2PayloadFields(void)
{
    static const ZyanU8 bytes[] = { 0xD5, 0x58, 0x89, 0xD1 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.attributes & ZYDIS_ATTRIB_HAS_REX2);
    TASK_CHECK(ins.raw.rex2.offset == 0);
    TASK_CHECK(ins.raw.rex2.payload == 0x58);
    TASK_CHECK(ins.raw.rex2.M0 == 0);
    TASK_CHECK(ins.raw.rex2.R4 == 1);
    TASK_CHECK(ins.raw.rex2.X4 == 0);
    TASK_CHECK(ins.raw.rex2.B4 == 1);
    TASK_CHECK(ins.raw.rex2.W == 1);
    TASK_CHECK(ins.raw.rex2.R3 == 0);
    TASK_CHECK(ins.raw.rex2.X3 == 0);
    TASK_CHECK(ins.raw.rex2.B3 == 0);
    TASK_CHECK(ins.length == 4);
    TASK_CHECK(ins.encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY);
    TASK_CHECK(ins.opcode_map == ZYDIS_OPCODE_MAP_DEFAULT);
    return 0;
}

static int TestRex2Map1Fields(void)
{
    static const ZyanU8 bytes[] = { 0xD5, 0xC8, 0x44, 0xC1 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.attributes & ZYDIS_ATTRIB_HAS_REX2);
    TASK_CHECK(ins.raw.rex2.payload == 0xC8);
    TASK_CHECK(ins.raw.rex2.M0 == 1);
    TASK_CHECK(ins.raw.rex2.R4 == 1);
    TASK_CHECK(ins.raw.rex2.B4 == 0);
    TASK_CHECK(ins.raw.rex2.W == 1);
    // `M0 == 1` selects map 1 without an additional `0F` byte.
    TASK_CHECK(ins.opcode_map == ZYDIS_OPCODE_MAP_0F);
    TASK_CHECK(ins.opcode == 0x44);
    TASK_CHECK(ins.length == 4);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_CMOVZ);
    return 0;
}

static int TestRex2AfterLegacyPrefixes(void)
{
    static const ZyanU8 bytes[] = { 0x66, 0x67, 0xD5, 0x50, 0x89, 0xC0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.attributes & ZYDIS_ATTRIB_HAS_REX2);
    TASK_CHECK(ins.attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE);
    TASK_CHECK(ins.attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE);
    TASK_CHECK(ins.raw.rex2.offset == 2);
    TASK_CHECK(ins.raw.rex2.payload == 0x50);
    TASK_CHECK(ins.raw.prefix_count == 2);
    TASK_CHECK(ins.raw.prefixes[0].value == 0x66);
    TASK_CHECK(ins.raw.prefixes[1].value == 0x67);
    TASK_CHECK(ins.raw.modrm.offset == 5);
    return 0;
}

static int TestRex2OpcodeExtensions(void)
{
    // Map-0 opcodes in the supported range.
    static const ZyanU8 mov[] = { 0xD5, 0x10, 0x88, 0xC0 };
    static const ZyanU8 lea[] = { 0xD5, 0x10, 0x8D, 0x00 };
    static const ZyanU8 add[] = { 0xD5, 0x10, 0x03, 0xC0 };
    static const ZyanU8 sub[] = { 0xD5, 0x10, 0x2B, 0xC0 };
    static const ZyanU8 cmp[] = { 0xD5, 0x10, 0x3B, 0xC0 };
    static const ZyanU8 seto[] = { 0xD5, 0xD0, 0x90, 0xC0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    CHECK_BOTH(mov, sizeof(mov), &ins, ops);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_MOV);
    CHECK_BOTH(lea, sizeof(lea), &ins, ops);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_LEA);
    CHECK_BOTH(add, sizeof(add), &ins, ops);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_ADD);
    CHECK_BOTH(sub, sizeof(sub), &ins, ops);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_SUB);
    CHECK_BOTH(cmp, sizeof(cmp), &ins, ops);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_CMP);
    CHECK_BOTH(seto, sizeof(seto), &ins, ops);
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_SETO);
    return 0;
}

static int TestUnsupportedRex2Opcodes(void)
{
    // `8F` is not part of the supported `REX2` subset.
    static const ZyanU8 unsupported_map0[] = { 0xD5, 0x18, 0x8F, 0xC0 };
    // `0F` would require an additional `0F` byte and is not a valid `REX2` opcode.
    static const ZyanU8 unsupported_map1[] = { 0xD5, 0x98, 0x0F, 0xC0 };
    // A legacy prefix byte directly after a map-0 `REX2` prefix is invalid.
    static const ZyanU8 prefix_after_rex2[] = { 0xD5, 0x18, 0x66, 0x89, 0xC0 };
    // A legacy `REX` prefix immediately followed by `D5` is illegal.
    static const ZyanU8 rex_before_rex2[] = { 0x48, 0xD5, 0x58, 0x89, 0xD1 };
    // A second `REX2` prefix is invalid.
    static const ZyanU8 double_rex2[] = { 0xD5, 0x18, 0xD5, 0x18, 0x89, 0xC0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    TASK_CHECK(!ZYAN_SUCCESS(DecodeFull64(unsupported_map0, sizeof(unsupported_map0), &ins,
        ops)));
    TASK_CHECK(!ZYAN_SUCCESS(DecodeFull64(unsupported_map1, sizeof(unsupported_map1), &ins,
        ops)));
    TASK_CHECK(!ZYAN_SUCCESS(DecodeFull64(prefix_after_rex2, sizeof(prefix_after_rex2), &ins,
        ops)));
    TASK_CHECK(ZydisDecoderDecodeFull(&(ZydisDecoder){ 0 }, unsupported_map0, 0, &ins,
        ops) != ZYAN_STATUS_SUCCESS);
    TASK_CHECK(!ZYAN_SUCCESS(DecodeFull64(rex_before_rex2, sizeof(rex_before_rex2), &ins, ops)));
    TASK_CHECK(!ZYAN_SUCCESS(DecodeFull64(double_rex2, sizeof(double_rex2), &ins, ops)));

    // A failed call must not leave a broken decoder: a subsequent valid call must succeed.
    {
        ZydisDecoder decoder;
        TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
            ZYDIS_STACK_WIDTH_64)));
        ZydisDecodedInstruction tmp_ins;
        ZydisDecodedOperand tmp_ops[ZYDIS_MAX_OPERAND_COUNT];
        TASK_CHECK(!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, unsupported_map0,
            sizeof(unsupported_map0), &tmp_ins, tmp_ops)));
        static const ZyanU8 valid[] = { 0xD5, 0x58, 0x89, 0xD1 };
        TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, valid, sizeof(valid),
            &tmp_ins, tmp_ops)));
        TASK_CHECK(tmp_ins.length == sizeof(valid));
    }
    return 0;
}

static int TestTruncatedRex2(void)
{
    static const struct
    {
        ZyanU8 bytes[16];
        ZyanU8 length;
    } cases[] =
    {
        { { 0xD5 },                             1 },  // missing payload
        { { 0xD5, 0x58 },                       2 },  // missing opcode
        { { 0xD5, 0x58, 0x89 },                 3 },  // missing ModRM
        { { 0xD5, 0x18, 0x8B, 0x44 },           4 },  // missing SIB byte
        { { 0xD5, 0x18, 0x8B, 0x44, 0xA3 },     5 },  // missing disp8
        { { 0xD5, 0x18, 0xB8, 1, 2, 3, 4 },     7 },  // missing bytes of imm64
        { { 0xD5 },                             0 }   // zero length
    };

    for (ZyanUSize i = 0; i < ARRAY_LENGTH(cases); ++i)
    {
        ZydisDecoder decoder;
        TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
            ZYDIS_STACK_WIDTH_64)));
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        const ZyanStatus status = ZydisDecoderDecodeFull(&decoder, cases[i].bytes,
            cases[i].length, &ins, ops);
        TASK_CHECK(!ZYAN_SUCCESS(status));
    }
    return 0;
}

static int TestRex2DoesNotChangeLegacyAAD(void)
{
    static const ZyanU8 aad[] = { 0xD5, 0x0A };
    ZydisDecoder decoder;
    TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_32,
        ZYDIS_STACK_WIDTH_32)));
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, aad, sizeof(aad), &ins, ops)));
    TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_AAD);
    TASK_CHECK(ins.length == 2);
    TASK_CHECK(!(ins.attributes & ZYDIS_ATTRIB_HAS_REX2));

    ZydisDecodedInstruction ins16;
    ZydisDecodedOperand ops16[ZYDIS_MAX_OPERAND_COUNT];
    TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LEGACY_16,
        ZYDIS_STACK_WIDTH_16)));
    TASK_CHECK(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, aad, sizeof(aad), &ins16, ops16)));
    TASK_CHECK(ins16.mnemonic == ZYDIS_MNEMONIC_AAD);
    TASK_CHECK(ins16.length == 2);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* Registers                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

static int TestExtendedRegisterMetadata(void)
{
    // GPR8 keeps ids 0..19 for the original registers and assigns ids 20..35 to R16B..R31B.
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_AL) == 0);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_AH) == 4);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_SPL) == 8);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R15B) == 19);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R16B) == 20);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R17B) == 21);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R31B) == 35);

    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R16W) == 16);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R16D) == 16);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R16) == 16);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R31) == 31);
    TASK_CHECK(ZydisRegisterGetId(ZYDIS_REGISTER_R15) == 15);

    TASK_CHECK(ZydisRegisterGetClass(ZYDIS_REGISTER_R16B) == ZYDIS_REGCLASS_GPR8);
    TASK_CHECK(ZydisRegisterGetClass(ZYDIS_REGISTER_R16W) == ZYDIS_REGCLASS_GPR16);
    TASK_CHECK(ZydisRegisterGetClass(ZYDIS_REGISTER_R16D) == ZYDIS_REGCLASS_GPR32);
    TASK_CHECK(ZydisRegisterGetClass(ZYDIS_REGISTER_R16) == ZYDIS_REGCLASS_GPR64);

    TASK_CHECK(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_REGISTER_R31B) == 8);
    TASK_CHECK(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_REGISTER_R31W) == 16);
    TASK_CHECK(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_REGISTER_R31D) == 32);
    TASK_CHECK(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_REGISTER_R31) == 64);

    TASK_CHECK(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_REGISTER_R16B) == ZYDIS_REGISTER_R16);
    TASK_CHECK(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_REGISTER_R31W) == ZYDIS_REGISTER_R31);
    TASK_CHECK(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_REGISTER_R31D) == ZYDIS_REGISTER_R31);
    TASK_CHECK(ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LEGACY_32,
        ZYDIS_REGISTER_R16B) == ZYDIS_REGISTER_R16D);

    TASK_CHECK(strcmp(ZydisRegisterGetString(ZYDIS_REGISTER_R16B), "r16b") == 0);
    TASK_CHECK(strcmp(ZydisRegisterGetString(ZYDIS_REGISTER_R31) , "r31") == 0);
    TASK_CHECK(strcmp(ZydisRegisterGetString(ZYDIS_REGISTER_R31D), "r31d") == 0);

    // Encode/GetId round-trips, including the appended (non-contiguous) enum ranges.
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, 0) == ZYDIS_REGISTER_AL);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, 4) == ZYDIS_REGISTER_AH);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, 8) == ZYDIS_REGISTER_SPL);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, 20) == ZYDIS_REGISTER_R16B);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, 35) == ZYDIS_REGISTER_R31B);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, 16) == ZYDIS_REGISTER_R16);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, 31) == ZYDIS_REGISTER_R31);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR16, 16) == ZYDIS_REGISTER_R16W);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, 31) == ZYDIS_REGISTER_R31D);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, 36) == ZYDIS_REGISTER_NONE);
    TASK_CHECK(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, 32) == ZYDIS_REGISTER_NONE);

    for (ZyanU8 id = 0; id <= 35; ++id)
    {
        const ZydisRegister reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR8, id);
        TASK_CHECK(reg != ZYDIS_REGISTER_NONE);
        TASK_CHECK(ZydisRegisterGetId(reg) == (ZyanI8)id);
    }
    for (ZyanU8 id = 0; id <= 31; ++id)
    {
        const ZydisRegister reg = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, id);
        TASK_CHECK(reg != ZYDIS_REGISTER_NONE);
        TASK_CHECK(ZydisRegisterGetId(reg) == (ZyanI8)id);
    }

    // The pre-existing enum values stay untouched.
    TASK_CHECK(ZYDIS_REGISTER_AL == 1);
    TASK_CHECK(ZYDIS_REGISTER_AH == 5);
    TASK_CHECK(ZYDIS_REGISTER_R15B == 20);
    TASK_CHECK(ZYDIS_REGISTER_AX == 21);
    TASK_CHECK(ZYDIS_REGISTER_EAX == 37);
    TASK_CHECK(ZYDIS_REGISTER_RAX == 53);
    TASK_CHECK(ZYDIS_REGISTER_R15 == 68);
    return 0;
}

static int TestExtendedRegisterDecoding(void)
{
    // `mov r17, r18`
    static const ZyanU8 bytes[] = { 0xD5, 0x58, 0x89, 0xD1 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.operand_count_visible == 2);
    TASK_CHECK(ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
    TASK_CHECK(ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R17);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_R18);
    TASK_CHECK(ops[0].actions == ZYDIS_OPERAND_ACTION_WRITE);
    TASK_CHECK(ops[1].actions == ZYDIS_OPERAND_ACTION_READ);
    TASK_CHECK(ops[0].size == 64);
    TASK_CHECK(ops[1].size == 64);
    TASK_CHECK(ZydisRegisterGetClass(ops[0].reg.value) == ZYDIS_REGCLASS_GPR64);
    CHECK_TEXT(bytes, sizeof(bytes), 0x401000, "mov r17, r18", "mov %r18, %r17");
    return 0;
}

static int TestExtendedByteRegisters(void)
{
    // `mov r24b, r26b`: payload 0x55 sets both R4 (reg) and B4 (rm) plus R3/B3.
    static const ZyanU8 bytes[] = { 0xD5, 0x55, 0x88, 0xD0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.operand_width == 8);
    TASK_CHECK(ops[0].size == 8);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R24B);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_R26B);
    TASK_CHECK(ZydisRegisterGetId(ops[0].reg.value) == 28);
    TASK_CHECK(ZydisRegisterGetId(ops[1].reg.value) == 30);
    CHECK_TEXT(bytes, sizeof(bytes), 0x401000, "mov r24b, r26b", "mov %r26b, %r24b");
    return 0;
}

static int TestHighByteAliasing(void)
{
    // Legacy encoding without REX: rm == 4 addresses `AH`.
    static const ZyanU8 legacy[] = { 0x88, 0xC4 };
    // The same ModRM byte under `REX2` addresses `SPL` (ids 4..7 are never AH..BH under REX2).
    static const ZyanU8 rex2[] = { 0xD5, 0x00, 0x88, 0xC4 };
    // `REX` has the same aliasing rule.
    static const ZyanU8 rex[] = { 0x40, 0x88, 0xC4 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    TASK_CHECK(ZYAN_SUCCESS(DecodeFull64(legacy, sizeof(legacy), &ins, ops)));
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_AH);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_AL);
    CHECK_TEXT(legacy, sizeof(legacy), 0, "mov ah, al", "mov %al, %ah");

    CHECK_BOTH(rex2, sizeof(rex2), &ins, ops);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_SPL);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_AL);
    CHECK_TEXT(rex2, sizeof(rex2), 0, "mov spl, al", "mov %al, %spl");

    TASK_CHECK(ZYAN_SUCCESS(DecodeFull64(rex, sizeof(rex), &ins, ops)));
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_SPL);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_AL);
    return 0;
}

static int TestExtendedOperandWidths(void)
{
    // `66` selects 16-bit operands for `mov r16w, r16w`.
    static const ZyanU8 bytes66[] = { 0x66, 0xD5, 0x50, 0x89, 0xC0 };
    // Without `66`/`W` the default operand size is 32 bits.
    static const ZyanU8 bytes32[] = { 0xD5, 0x50, 0x89, 0xC0 };
    // `W` promotes to 64 bits.
    static const ZyanU8 bytes64[] = { 0xD5, 0x58, 0x89, 0xC0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    CHECK_BOTH(bytes66, sizeof(bytes66), &ins, ops);
    TASK_CHECK(ins.operand_width == 16);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16W);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_R16W);
    CHECK_TEXT(bytes66, sizeof(bytes66), 0, "mov r16w, r16w", "mov %r16w, %r16w");

    CHECK_BOTH(bytes32, sizeof(bytes32), &ins, ops);
    TASK_CHECK(ins.operand_width == 32);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16D);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_R16D);

    CHECK_BOTH(bytes64, sizeof(bytes64), &ins, ops);
    TASK_CHECK(ins.operand_width == 64);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_R16);
    return 0;
}

static int TestImmediateForms(void)
{
    // `mov r16b, imm8`
    static const ZyanU8 imm8[] = { 0xD5, 0x50, 0xB0, 0x7F };
    // `mov r16, imm64` (`W == 1`)
    static const ZyanU8 imm64[] = { 0xD5, 0x18, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    CHECK_BOTH(imm8, sizeof(imm8), &ins, ops);
    TASK_CHECK(ins.operand_count_visible == 2);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16B);
    TASK_CHECK(ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);
    TASK_CHECK(ops[1].imm.value.u == 0x7F);
    TASK_CHECK(ins.raw.imm[0].offset == 3);
    TASK_CHECK(ins.raw.imm[0].size == 8);
    CHECK_TEXT(imm8, sizeof(imm8), 0, "mov r16b, 0x7F", "mov $0x7F, %r16b");

    CHECK_BOTH(imm64, sizeof(imm64), &ins, ops);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16);
    TASK_CHECK(ops[1].imm.value.u == UINT64_C(0x0807060504030201));
    TASK_CHECK(ins.raw.imm[0].offset == 3);
    TASK_CHECK(ins.raw.imm[0].size == 64);
    TASK_CHECK(ins.length == 11);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* Addressing                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

static int TestExtendedSib(void)
{
    // `mov eax, [r19+r20*4+0x10]`: REX2 sets B4 (base) and X4 (index) for the SIB byte.
    static const ZyanU8 bytes[] = { 0xD5, 0x38, 0x8B, 0x44, 0xA3, 0x10 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_RAX);
    TASK_CHECK(ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_R19);
    TASK_CHECK(ops[1].mem.index == ZYDIS_REGISTER_R20);
    TASK_CHECK(ops[1].mem.scale == 4);
    TASK_CHECK(ops[1].mem.disp.has_displacement);
    TASK_CHECK(ops[1].mem.disp.value == 0x10);
    TASK_CHECK(ins.raw.sib.offset == 4);
    TASK_CHECK(ins.raw.disp.offset == 5);
    TASK_CHECK(ins.raw.disp.size == 8);
    CHECK_TEXT(bytes, sizeof(bytes), 0x401000, "mov rax, [r19+r20*4+0x10]",
        "mov 0x10(%r19,%r20,4), %rax");
    return 0;
}

static int TestSibNoIndexSentinel(void)
{
    // `SIB.index == 4` denotes "no index" only when the full extended id is 4.
    static const ZyanU8 no_index[] = { 0xD5, 0x10, 0x8B, 0x04, 0x20, 0x10, 0, 0, 0 };
    // `SIB.index` raw id 4 extended to `R20` by `X4` is a real index register.
    static const ZyanU8 extended_index[] = { 0xD5, 0x30, 0x8B, 0x04, 0xA5, 0x10, 0, 0, 0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    CHECK_BOTH(no_index, sizeof(no_index), &ins, ops);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_R16);
    TASK_CHECK(ops[1].mem.index == ZYDIS_REGISTER_NONE);
    TASK_CHECK(ops[1].mem.scale == 0);

    CHECK_BOTH(extended_index, sizeof(extended_index), &ins, ops);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_NONE);
    TASK_CHECK(ops[1].mem.index == ZYDIS_REGISTER_R20);
    TASK_CHECK(ops[1].mem.scale == 4);
    TASK_CHECK(ops[1].mem.disp.value == 0x10);
    return 0;
}

static int TestSibBaseSentinel(void)
{
    // `mod == 0` with `SIB.base == 5` means "no base" and consumes a `disp32`, even when the
    // `REX2` base extension bits are set.
    static const ZyanU8 bytes[] = { 0xD5, 0x11, 0x8B, 0x04, 0x25, 0x10, 0, 0, 0 };
    // With `mod == 1` the same base bits address `R29`.
    static const ZyanU8 bytes_mod1[] = { 0xD5, 0x11, 0x8B, 0x44, 0x25, 0x10 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_NONE);
    TASK_CHECK(ops[1].mem.index == ZYDIS_REGISTER_NONE);
    TASK_CHECK(ops[1].mem.disp.has_displacement);
    TASK_CHECK(ops[1].mem.disp.value == 0x10);
    TASK_CHECK(ins.raw.disp.size == 32);

    CHECK_BOTH(bytes_mod1, sizeof(bytes_mod1), &ins, ops);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_R29);
    TASK_CHECK(ops[1].mem.disp.value == 0x10);
    return 0;
}

static int TestAddressSizeOverride(void)
{
    // `67` makes the effective address 32 bits wide; the extended registers keep their `*D` name.
    static const ZyanU8 bytes[] = { 0x67, 0xD5, 0x50, 0x8B, 0x00 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.address_width == 32);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16D);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_R16D);
    CHECK_TEXT(bytes, sizeof(bytes), 0, "mov r16d, [r16d]", "mov (%r16d), %r16d");
    return 0;
}

static int TestRipRelative(void)
{
    // `lea r16, [rip+0x10]` at 0x401000: the target is relative to the end of the instruction.
    static const ZyanU8 bytes[] = { 0xD5, 0x48, 0x8D, 0x05, 0x10, 0, 0, 0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);

    TASK_CHECK(ins.length == 8);
    TASK_CHECK(ins.raw.disp.offset == 4);
    TASK_CHECK(ins.raw.disp.size == 32);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_RIP);
    TASK_CHECK(ops[1].mem.disp.value == 0x10);

    ZyanU64 address = 0;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &ops[1], 0x401000, &address)));
    TASK_CHECK(address == 0x401018);
    CHECK_TEXT(bytes, sizeof(bytes), 0x401000, "lea r16, [0x0000000000401018]",
        "lea 0x0000000000401018, %r16");
    return 0;
}

static int TestAbsoluteAddressWithExtendedRegisters(void)
{
    // `mov eax, [r16+r17*2+0x10]`
    static const ZyanU8 bytes[] = { 0xD5, 0x30, 0x8B, 0x44, 0x48, 0x10 };
    // `67 mov eax, [r16d+r17d*2+0x10]`: the whole sum is truncated to 32 bits.
    static const ZyanU8 bytes67[] = { 0x67, 0xD5, 0x30, 0x8B, 0x44, 0x48, 0x10 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    ZydisRegisterContext context;

    CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_R16);
    TASK_CHECK(ops[1].mem.index == ZYDIS_REGISTER_R17);
    TASK_CHECK(ops[1].mem.scale == 2);

    memset(&context, 0, sizeof(context));
    context.values[ZYDIS_REGISTER_R16] = UINT64_C(0x1122334455667700);
    context.values[ZYDIS_REGISTER_R17] = UINT64_C(0x20);
    // Also set the 32-bit variants to different values to prove the true operand registers are
    // used.
    context.values[ZYDIS_REGISTER_R16D] = UINT64_C(0xDEADBEEF);
    context.values[ZYDIS_REGISTER_R17D] = UINT64_C(0xFEEDFACE);
    ZyanU64 address = 0;
    TASK_CHECK(ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&ins, &ops[1], 0x401000, &context,
        &address)));
    TASK_CHECK(address == (UINT64_C(0x1122334455667700) + (UINT64_C(0x20) * 2) + 0x10));

    CHECK_BOTH(bytes67, sizeof(bytes67), &ins, ops);
    TASK_CHECK(ins.address_width == 32);
    TASK_CHECK(ops[1].mem.base == ZYDIS_REGISTER_R16D);
    TASK_CHECK(ops[1].mem.index == ZYDIS_REGISTER_R17D);
    TASK_CHECK(ops[1].mem.scale == 2);
    TASK_CHECK(ops[1].mem.disp.value == 0x10);

    memset(&context, 0, sizeof(context));
    // Both registers are wider than 32 bits; the truncation must happen after the complete sum.
    context.values[ZYDIS_REGISTER_R16D] = UINT64_C(0xFFFFFFFF00000010);
    context.values[ZYDIS_REGISTER_R17D] = UINT64_C(0x0000000200000003);
    // The 64-bit entries are deliberately different and must not be read.
    context.values[ZYDIS_REGISTER_R16] = UINT64_C(0xAAAAAAAAAAAAAAAA);
    context.values[ZYDIS_REGISTER_R17] = UINT64_C(0xBBBBBBBBBBBBBBBB);
    TASK_CHECK(ZYAN_SUCCESS(ZydisCalcAbsoluteAddressEx(&ins, &ops[1], 0x401000, &context,
        &address)));
    // (0xFFFFFFFF00000010 + 3 * 2 + 0x10) truncated to 32 bits == 0x26.
    TASK_CHECK(address == 0x26);
    return 0;
}

static int TestRex2OperandAccessAndFlags(void)
{
    // The `REX2` forms keep the operand access and flag semantics of the legacy instructions.
    static const ZyanU8 add[] = { 0xD5, 0x18, 0x01, 0xC0 };
    static const ZyanU8 cmp[] = { 0xD5, 0x18, 0x39, 0xC0 };
    static const ZyanU8 cmov[] = { 0xD5, 0xC8, 0x44, 0xC1 };
    static const ZyanU8 seto[] = { 0xD5, 0xD0, 0x90, 0xC0 };
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

    CHECK_BOTH(add, sizeof(add), &ins, ops);
    TASK_CHECK(ops[0].actions == ZYDIS_OPERAND_ACTION_READWRITE);
    TASK_CHECK(ops[1].actions == ZYDIS_OPERAND_ACTION_READ);
    TASK_CHECK(ins.operand_count_visible == 2);

    CHECK_BOTH(cmp, sizeof(cmp), &ins, ops);
    TASK_CHECK(ops[0].actions == ZYDIS_OPERAND_ACTION_READ);
    TASK_CHECK(ops[1].actions == ZYDIS_OPERAND_ACTION_READ);

    CHECK_BOTH(cmov, sizeof(cmov), &ins, ops);
    TASK_CHECK(ops[0].actions == ZYDIS_OPERAND_ACTION_CONDWRITE);
    TASK_CHECK(ops[1].actions == ZYDIS_OPERAND_ACTION_READ);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16);
    TASK_CHECK(ops[1].reg.value == ZYDIS_REGISTER_RCX);

    // `SETcc` uses a byte r/m destination whose ModRM.reg bits are ignored. All eight possible
    // `ModRM.reg` values must decode to the same operand.
    CHECK_BOTH(seto, sizeof(seto), &ins, ops);
    TASK_CHECK(ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
    TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16B);
    TASK_CHECK(ops[0].size == 8);
    TASK_CHECK(ops[0].actions == ZYDIS_OPERAND_ACTION_WRITE);
    for (ZyanU8 reg = 0; reg < 8; ++reg)
    {
        static const ZyanU8 template_bytes[] = { 0xD5, 0xD0, 0x90, 0xC0 };
        ZyanU8 bytes[sizeof(template_bytes)];
        memcpy(bytes, template_bytes, sizeof(bytes));
        bytes[3] = (ZyanU8)(0xC0 | (reg << 3));
        CHECK_BOTH(bytes, sizeof(bytes), &ins, ops);
        TASK_CHECK(ins.mnemonic == ZYDIS_MNEMONIC_SETO);
        TASK_CHECK(ops[0].reg.value == ZYDIS_REGISTER_R16B);
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* Case table                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

const struct task_case task_cases[] =
{
    { "prefixes",  "rex2_payload_fields",          &TestRex2PayloadFields            },
    { "prefixes",  "rex2_map1_fields",             &TestRex2Map1Fields               },
    { "prefixes",  "rex2_after_legacy_prefixes",   &TestRex2AfterLegacyPrefixes      },
    { "prefixes",  "rex2_opcode_extensions",       &TestRex2OpcodeExtensions         },
    { "prefixes",  "unsupported_rex2_opcodes",     &TestUnsupportedRex2Opcodes       },
    { "prefixes",  "truncated_rex2",               &TestTruncatedRex2                },
    { "prefixes",  "rex2_does_not_change_aad",     &TestRex2DoesNotChangeLegacyAAD   },
    { "registers", "extended_register_metadata",   &TestExtendedRegisterMetadata     },
    { "registers", "extended_register_decoding",   &TestExtendedRegisterDecoding     },
    { "registers", "extended_byte_registers",      &TestExtendedByteRegisters        },
    { "registers", "high_byte_aliasing",           &TestHighByteAliasing             },
    { "registers", "extended_operand_widths",      &TestExtendedOperandWidths        },
    { "registers", "immediate_forms",              &TestImmediateForms               },
    { "addressing", "extended_sib",                &TestExtendedSib                  },
    { "addressing", "sib_no_index_sentinel",       &TestSibNoIndexSentinel           },
    { "addressing", "sib_base_sentinel",           &TestSibBaseSentinel              },
    { "addressing", "address_size_override",       &TestAddressSizeOverride          },
    { "addressing", "rip_relative",                &TestRipRelative                  },
    { "addressing", "absolute_address_extended",   &TestAbsoluteAddressWithExtendedRegisters },
    { "addressing", "operand_access_and_flags",    &TestRex2OperandAccessAndFlags    },
};

const size_t task_case_count = ARRAY_LENGTH(task_cases);
