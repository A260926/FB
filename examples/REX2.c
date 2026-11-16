/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Demonstrates decoding and formatting `APX` `REX2`-encoded instructions (`D5` prefix) with the
 * ordinary decoder and formatter APIs. The bytes are treated as read-only input data and are
 * never executed.
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <Zydis/Zydis.h>

static const struct
{
    const char* name;
    ZyanU8 bytes[16];
    ZyanU8 length;
} REX2_EXAMPLES[] =
{
    { "MOV r17, r18 (REX2 map 0)",          { 0xD5, 0x58, 0x89, 0xD1 },                   4 },
    { "MOV rax, [r19+r20*4+0x10] (SIB)",    { 0xD5, 0x38, 0x8B, 0x44, 0xA3, 0x10 },        6 },
    { "CMOVZ r16, rcx (REX2 map 1)",        { 0xD5, 0xC8, 0x44, 0xC1 },                   4 },
    { "MOV r16w, r16w (66 override)",       { 0x66, 0xD5, 0x50, 0x89, 0xC0 },             5 },
    { "MOV r16d, [r16d] (67 override)",     { 0x67, 0xD5, 0x50, 0x8B, 0x00 },             5 },
    { "SETO r16b",                          { 0xD5, 0xD0, 0x90, 0xC0 },                   4 },
    { "LEA r16, [rip+0x10]",                { 0xD5, 0x48, 0x8D, 0x05, 0x10, 0, 0, 0 },    8 },
};

int main(void)
{
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
        ZYDIS_STACK_WIDTH_64)))
    {
        fprintf(stderr, "Failed to initialize the decoder\n");
        return 1;
    }

    ZydisFormatter intel;
    ZydisFormatter att;
    ZydisFormatterInit(&intel, ZYDIS_FORMATTER_STYLE_INTEL);
    ZydisFormatterInit(&att, ZYDIS_FORMATTER_STYLE_ATT);

    // The runtime address is only used to resolve `RIP`-relative operands for display purposes.
    const ZyanU64 runtime_address = 0x401000;

    for (ZyanUSize i = 0; i < sizeof(REX2_EXAMPLES) / sizeof(REX2_EXAMPLES[0]); ++i)
    {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, REX2_EXAMPLES[i].bytes,
            REX2_EXAMPLES[i].length, &instruction, operands)))
        {
            printf("%-36s : <decoding failed>\n", REX2_EXAMPLES[i].name);
            continue;
        }

        char intel_text[256];
        char att_text[256];
        ZydisFormatterFormatInstruction(&intel, &instruction, operands,
            instruction.operand_count_visible, intel_text, sizeof(intel_text), runtime_address,
            ZYAN_NULL);
        ZydisFormatterFormatInstruction(&att, &instruction, operands,
            instruction.operand_count_visible, att_text, sizeof(att_text), runtime_address,
            ZYAN_NULL);

        printf("%s\n", REX2_EXAMPLES[i].name);
        printf("  intel : %s\n", intel_text);
        printf("  att   : %s\n", att_text);
        printf("  bytes :");
        for (ZyanU8 j = 0; j < instruction.length; ++j)
        {
            printf(" %02X", REX2_EXAMPLES[i].bytes[j]);
        }
        printf("\n  rex2  : offset=%u payload=0x%02X M0=%u R4=%u X4=%u B4=%u W=%u R3=%u X3=%u "
            "B3=%u\n", instruction.raw.rex2.offset, instruction.raw.rex2.payload,
            instruction.raw.rex2.M0, instruction.raw.rex2.R4, instruction.raw.rex2.X4,
            instruction.raw.rex2.B4, instruction.raw.rex2.W, instruction.raw.rex2.R3,
            instruction.raw.rex2.X3, instruction.raw.rex2.B3);
        printf("\n");
    }

    return 0;
}
