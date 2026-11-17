/*
 * Copyright 2024 Tadashi G. Takaoka
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Regression for the structured MC68000/MC68020 integer memory operand
// model exposed through Insn::memoryOperands(). Every check goes through
// TASK_CHECK so task_support/driver.c can account for it.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "task_support/check.h"

#include "array_memory.h"
#include "dis_mc68000.h"

using namespace libasm;
using namespace libasm::mc68000;

namespace {

constexpr uint32_t ORIGIN = 0x1000;

// Decode a big-endian 16-bit word stream. The ArrayMemory dies on return,
// so reading the model afterwards also proves the Insn owns its values.
// Mirrors the disassembler driver contract: the Insn is reset per decode.
Error decode16(Disassembler &dis, Insn &insn, char *text, size_t text_size, const uint16_t *words,
        size_t nwords, uint32_t origin = ORIGIN) {
    insn.reset(origin);
    const ArrayMemory memory(origin, words, nwords * 2, ENDIAN_BIG, ADDRESS_BYTE);
    auto cursor = memory.iterator();
    return dis.decode(cursor, insn, text, text_size);
}

DisMc68000 *new68020(DisMc68000 &dis) {
    Disassembler &base = dis;
    base.setCpu("68020");
    return &dis;
}

bool sameOperand(const MemoryOperand &a, const MemoryOperand &b) {
    return a.operandIndex == b.operandIndex && a.elementSize == b.elementSize &&
           a.kind == b.kind && a.baseRegister == b.baseRegister &&
           a.baseSuppressed == b.baseSuppressed && a.indexRegister == b.indexRegister &&
           a.indexSuppressed == b.indexSuppressed && a.indexSize == b.indexSize &&
           a.indexScale == b.indexScale && a.pcBase == b.pcBase &&
           a.predecrement == b.predecrement && a.postincrement == b.postincrement &&
           a.baseDispSize == b.baseDispSize && a.baseDisp == b.baseDisp &&
           a.outerDispSize == b.outerDispSize && a.outerDisp == b.outerDisp &&
           a.absoluteShort == b.absoluteShort;
}

// ---- structure group -----------------------------------------------------

int structure_indirect_modes() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t aind[] = {0x2010};  // MOVE.L (A0), D0
    TASK_CHECK(decode16(dis, insn, text, sizeof text, aind, 1) == OK);
    TASK_CHECK(strcmp(text, "(A0), D0") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 1);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.operandIndex == 0);
    TASK_CHECK(m0.elementSize == 4);
    TASK_CHECK(m0.kind == MemoryOperand::INDIRECT);
    TASK_CHECK(m0.baseRegister == REG_A0);
    TASK_CHECK(!m0.baseSuppressed && !m0.predecrement && !m0.postincrement);
    TASK_CHECK(m0.indexRegister == -1 && m0.indexSize == MemoryOperand::INDEX_NONE);
    TASK_CHECK(m0.baseDispSize == MemoryOperand::DISP_NULL && m0.baseDisp == 0);
    TASK_CHECK(!m0.pcBase);

    const uint16_t pinc[] = {0x261A};  // MOVE.L (A2)+, D3
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pinc, 1) == OK);
    TASK_CHECK(strcmp(text, "(A2)+, D3") == 0);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(insn.memoryOperandCount() == 1);
    TASK_CHECK(m1.kind == MemoryOperand::POSTINCREMENT && m1.postincrement);
    TASK_CHECK(m1.baseRegister == REG_A2 && !m1.predecrement);

    const uint16_t pdec[] = {0x2622};  // MOVE.L -(A2), D3
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pdec, 1) == OK);
    TASK_CHECK(strcmp(text, "-(A2), D3") == 0);
    const auto &m2 = insn.memoryOperands()[0];
    TASK_CHECK(m2.kind == MemoryOperand::PREDECREMENT && m2.predecrement);
    TASK_CHECK(m2.baseRegister == REG_A2 && !m2.postincrement);
    return 0;
}

int structure_element_sizes() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t bytes[] = {0x1010};  // MOVE.B (A0), D0
    TASK_CHECK(decode16(dis, insn, text, sizeof text, bytes, 1) == OK);
    TASK_CHECK(strstr(text, "(A0)") != nullptr);
    TASK_CHECK(insn.memoryOperands()[0].elementSize == 1);

    const uint16_t words[] = {0x3010};  // MOVE.W (A0), D0
    TASK_CHECK(decode16(dis, insn, text, sizeof text, words, 1) == OK);
    TASK_CHECK(insn.memoryOperands()[0].elementSize == 2);

    const uint16_t longs[] = {0x2010};  // MOVE.L (A0), D0
    TASK_CHECK(decode16(dis, insn, text, sizeof text, longs, 1) == OK);
    TASK_CHECK(insn.memoryOperands()[0].elementSize == 4);
    return 0;
}

int structure_displacement_and_absolute() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t d16[] = {0x2028, 0xFEDC};  // MOVE.L (-$24,A0), D0
    TASK_CHECK(decode16(dis, insn, text, sizeof text, d16, 2) == OK);
    TASK_CHECK(strstr(text, "A0") != nullptr);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.kind == MemoryOperand::DISPLACEMENT);
    TASK_CHECK(m0.baseRegister == REG_A0 && !m0.pcBase);
    TASK_CHECK(m0.baseDispSize == MemoryOperand::DISP_WORD);
    TASK_CHECK(m0.baseDisp == -292);

    const uint16_t abs_short[] = {0x2E38, 0xFEDC};  // MOVE.L ($FFFEDC).W, D7
    TASK_CHECK(decode16(dis, insn, text, sizeof text, abs_short, 2) == OK);
    TASK_CHECK(strcmp(text, "($FFFFFEDC).W, D7") == 0);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(m1.kind == MemoryOperand::ABSOLUTE && m1.absoluteShort);
    TASK_CHECK(m1.baseRegister == -1 && !m1.baseSuppressed && !m1.pcBase);
    TASK_CHECK(m1.baseDispSize == MemoryOperand::DISP_WORD);
    TASK_CHECK(m1.baseDisp == -292);  // sign-extended 16-bit value

    const uint16_t abs_long[] = {0x2E39, 0x1234, 0x5678};  // MOVE.L ($12345678).L, D7
    TASK_CHECK(decode16(dis, insn, text, sizeof text, abs_long, 3) == OK);
    TASK_CHECK(strcmp(text, "($12345678).L, D7") == 0);
    const auto &m2 = insn.memoryOperands()[0];
    TASK_CHECK(m2.kind == MemoryOperand::ABSOLUTE && !m2.absoluteShort);
    TASK_CHECK(m2.baseDispSize == MemoryOperand::DISP_LONG);
    TASK_CHECK(m2.baseDisp == 0x12345678);  // raw 32-bit value, no sign extension
    return 0;
}

int structure_brief_extension() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // (18,A2,A3.L) via MOVEM.W: index A3, long, scale 1, disp8 = 0x12
    const uint16_t brief[] = {0044262, 0x007F, 0xB812};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, brief, 3) == OK);
    TASK_CHECK(strcmp(text, "D0-D6, (18,A2,A3.L)") == 0);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.kind == MemoryOperand::INDEXED);
    TASK_CHECK(m0.baseRegister == REG_A2);
    TASK_CHECK(m0.indexRegister == REG_A3);
    TASK_CHECK(m0.indexSize == MemoryOperand::INDEX_LONG && m0.indexScale == 1);
    TASK_CHECK(m0.baseDispSize == MemoryOperand::DISP_BYTE && m0.baseDisp == 0x12);
    TASK_CHECK(!m0.indexSuppressed && m0.outerDispSize == MemoryOperand::DISP_NULL);

    // MOVE.L (-1,A7,D1.W*8), D2: word index, scale 8, negative disp8
    const uint16_t scaled[] = {0022067, 0013000 | 0xFF};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, scaled, 2) == OK);
    TASK_CHECK(strstr(text, "D1.W*8") != nullptr);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(m1.kind == MemoryOperand::INDEXED);
    TASK_CHECK(m1.baseRegister == REG_A7 && m1.indexRegister == REG_D1);
    TASK_CHECK(m1.indexSize == MemoryOperand::INDEX_WORD && m1.indexScale == 8);
    TASK_CHECK(m1.baseDisp == -1);
    return 0;
}

int structure_pc_forms() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // LEA ($00002234,PC), A1: raw d16 = 0x1232
    const uint16_t pcdsp[] = {0041772, 0x1232};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pcdsp, 2) == OK);
    TASK_CHECK(strcmp(text, "($00002234,PC), A1") == 0);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.kind == MemoryOperand::PC_DISPLACEMENT);
    TASK_CHECK(m0.baseRegister == REG_PC && m0.pcBase && !m0.baseSuppressed);
    TASK_CHECK(m0.baseDispSize == MemoryOperand::DISP_WORD && m0.baseDisp == 0x1232);

    // Full extension pre-indexed PC form; the PC base is the extension word.
    const uint16_t pcfull[] = {0022073, 0010442, 0x1234, 0x5678};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pcfull, 4) == OK);
    TASK_CHECK(strcmp(text, "([$00002236,PC,D1.W],$5678), D2") == 0);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(m1.kind == MemoryOperand::MEMORY_PREINDEXED);
    TASK_CHECK(m1.pcBase && m1.baseRegister == REG_PC);
    TASK_CHECK(m1.baseDisp == 0x1234 && m1.baseDispSize == MemoryOperand::DISP_WORD);
    TASK_CHECK(m1.outerDisp == 0x5678 && m1.outerDispSize == MemoryOperand::DISP_WORD);
    TASK_CHECK(m1.indexRegister == REG_D1 && m1.indexScale == 1);
    return 0;
}

int structure_null_vs_zero_displacement() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // Null base displacement: no displacement word follows the extension.
    const uint16_t null_disp[] = {0022067, 0010420};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, null_disp, 2) == OK);
    TASK_CHECK(strcmp(text, "(A7,D1.W), D2") == 0);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.baseDispSize == MemoryOperand::DISP_NULL && m0.baseDisp == 0);
    TASK_CHECK(insn.length() == 4);

    // Encoded zero word displacement: same address meaning, distinct model.
    const uint16_t zero_disp[] = {0022067, 0010440, 0x0000};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, zero_disp, 3) == OK);
    TASK_CHECK(strcmp(text, "(0,A7,D1.W), D2") == 0);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(m1.baseDispSize == MemoryOperand::DISP_WORD && m1.baseDisp == 0);
    TASK_CHECK(insn.length() == 6);
    return 0;
}

int structure_full_extension_levels() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // Pre-indexed: Mem32[B+bd+I]+od
    const uint16_t pre[] = {0022067, 0010422, 0x1234};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pre, 3) == OK);
    TASK_CHECK(strcmp(text, "([A7,D1.W],$1234), D2") == 0);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.kind == MemoryOperand::MEMORY_PREINDEXED);
    TASK_CHECK(m0.baseRegister == REG_A7 && !m0.baseSuppressed);
    TASK_CHECK(m0.indexRegister == REG_D1 && !m0.indexSuppressed);
    TASK_CHECK(m0.baseDispSize == MemoryOperand::DISP_NULL);
    TASK_CHECK(m0.outerDispSize == MemoryOperand::DISP_WORD && m0.outerDisp == 0x1234);

    // Post-indexed: Mem32[B+bd]+I+od
    const uint16_t post[] = {0022067, 0010426, 0x1234};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, post, 3) == OK);
    TASK_CHECK(strcmp(text, "([A7],D1.W,$1234), D2") == 0);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(m1.kind == MemoryOperand::MEMORY_POSTINDEXED);
    TASK_CHECK(m1.baseRegister == REG_A7);
    TASK_CHECK(m1.indexRegister == REG_D1 && !m1.indexSuppressed);
    TASK_CHECK(m1.outerDisp == 0x1234);

    // Null outer displacement versus a 32-bit outer displacement.
    const uint16_t null_outer[] = {0022067, 0010425};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, null_outer, 2) == OK);
    TASK_CHECK(strcmp(text, "([A7],D1.W), D2") == 0);
    TASK_CHECK(insn.memoryOperands()[0].outerDispSize == MemoryOperand::DISP_NULL);
    TASK_CHECK(insn.memoryOperands()[0].outerDisp == 0);

    const uint16_t long_outer[] = {0022067, 0010427, 0x1234, 0x5678};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, long_outer, 4) == OK);
    TASK_CHECK(strstr(text, "$12345678") != nullptr);
    const auto &m3 = insn.memoryOperands()[0];
    TASK_CHECK(m3.outerDispSize == MemoryOperand::DISP_LONG);
    TASK_CHECK(m3.outerDisp == 0x12345678);
    return 0;
}

int structure_suppressed_components() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // Suppressed PC keeps its program-space origin instead of looking base-less.
    const uint16_t zpc[] = {0022073, 0010720};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, zpc, 2) == OK);
    TASK_CHECK(strcmp(text, "(ZPC), D2") == 0);
    const auto &m0 = insn.memoryOperands()[0];
    TASK_CHECK(m0.pcBase && m0.baseSuppressed && m0.baseRegister == -1);
    TASK_CHECK(m0.indexSuppressed && m0.indexRegister == -1);
    TASK_CHECK(m0.kind == MemoryOperand::PC_INDEXED);

    // Suppressed PC with a raw (non PC-relative) word displacement.
    const uint16_t zpc_disp[] = {0022073, 0010740, 0x1234};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, zpc_disp, 3) == OK);
    TASK_CHECK(strcmp(text, "($1234,ZPC), D2") == 0);
    const auto &m1 = insn.memoryOperands()[0];
    TASK_CHECK(m1.pcBase && m1.baseSuppressed);
    TASK_CHECK(m1.baseDispSize == MemoryOperand::DISP_WORD && m1.baseDisp == 0x1234);

    // Suppressed address base with a post-indexed register: no PC origin.
    const uint16_t no_base[] = {0022060, 0010625};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, no_base, 2) == OK);
    TASK_CHECK(strcmp(text, "([0],D1.W), D2") == 0);
    const auto &m2 = insn.memoryOperands()[0];
    TASK_CHECK(!m2.pcBase && m2.baseSuppressed && m2.baseRegister == -1);
    TASK_CHECK(m2.kind == MemoryOperand::MEMORY_POSTINDEXED);
    TASK_CHECK(m2.indexRegister == REG_D1 && !m2.indexSuppressed);
    return 0;
}

int structure_register_and_immediate_have_no_entry() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t reg_only[] = {0x2401};  // MOVE.L D1, D2
    TASK_CHECK(decode16(dis, insn, text, sizeof text, reg_only, 1) == OK);
    TASK_CHECK(strcmp(text, "D1, D2") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 0);

    const uint16_t immediate[] = {0x203C, 0x1234, 0x5678};  // MOVE.L #$12345678, D0
    TASK_CHECK(decode16(dis, insn, text, sizeof text, immediate, 3) == OK);
    TASK_CHECK(strcmp(text, "#$12345678, D0") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 0);
    return 0;
}

int structure_fpu_pmmu_excluded() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // FPU instruction with an integer memory effective address.
    const uint16_t fpu[] = {0xF216, 0x4B80};  // FMOVE.X (A6), FP7
    TASK_CHECK(decode16(dis, insn, text, sizeof text, fpu, 2) == OK);
    TASK_CHECK(strcmp(text, "(A6), FP7") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 0);

    // PMMU instruction with an integer memory effective address. The PMMU
    // table is selected through its option, not the CPU type alone.
    dis.setOption("pmmu", "on");
    const uint16_t pmmu[] = {0xF02A, 0x820B, 0x1234};  // PTESTR D3, ($1234,A2)
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pmmu, 3) == OK);
    TASK_CHECK(strstr(text, "A2") != nullptr);
    TASK_CHECK(insn.memoryOperandCount() == 0);
    dis.setOption("pmmu", "off");
    return 0;
}

int structure_display_options_invariant() {
    DisMc68000 defHolder;
    auto &defDis = *new68020(defHolder);
    DisMc68000 gnuHolder;
    auto &gnuDis = *new68020(gnuHolder);
    gnuDis.setOption("gnu-as", "on");
    DisMc68000 relHolder;
    auto &relDis = *new68020(relHolder);
    relDis.setOption("relative", "true");
    DisMc68000 spHolder;
    auto &spDis = *new68020(spHolder);
    spDis.setOption("sp-alias", "on");

    const uint16_t words[] = {0022067, 0010426, 0x1234};
    Insn a(ORIGIN), b(ORIGIN), c(ORIGIN), d(ORIGIN);
    char ta[160] = {0}, tb[160] = {0}, tc[160] = {0}, td[160] = {0};
    TASK_CHECK(decode16(defDis, a, ta, sizeof ta, words, 3) == OK);
    TASK_CHECK(decode16(gnuDis, b, tb, sizeof tb, words, 3) == OK);
    TASK_CHECK(decode16(relDis, c, tc, sizeof tc, words, 3) == OK);
    TASK_CHECK(decode16(spDis, d, td, sizeof td, words, 3) == OK);
    // The text changes with display options ...
    TASK_CHECK(strcmp(ta, tb) != 0 || strcmp(ta, tc) != 0 || strcmp(ta, td) != 0);
    TASK_CHECK(strstr(td, "SP") != nullptr);
    // ... but the structured model does not.
    TASK_CHECK(a.memoryOperandCount() == 1 && b.memoryOperandCount() == 1);
    TASK_CHECK(c.memoryOperandCount() == 1 && d.memoryOperandCount() == 1);
    TASK_CHECK(sameOperand(a.memoryOperands()[0], b.memoryOperands()[0]));
    TASK_CHECK(sameOperand(a.memoryOperands()[0], c.memoryOperands()[0]));
    TASK_CHECK(sameOperand(a.memoryOperands()[0], d.memoryOperands()[0]));
    return 0;
}

// ---- placement group -----------------------------------------------------

int placement_move_two_memory_operands() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t move[] = {0x2292};  // MOVE.L (A2), (A1)
    TASK_CHECK(decode16(dis, insn, text, sizeof text, move, 1) == OK);
    TASK_CHECK(strcmp(text, "(A2), (A1)") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 2);
    const auto &s = insn.memoryOperands()[0];
    const auto &d = insn.memoryOperands()[1];
    TASK_CHECK(s.operandIndex == 0 && s.kind == MemoryOperand::INDIRECT);
    TASK_CHECK(s.baseRegister == REG_A2);
    TASK_CHECK(d.operandIndex == 1 && d.kind == MemoryOperand::INDIRECT);
    TASK_CHECK(d.baseRegister == REG_A1);
    return 0;
}

int placement_movem_mask_before_extension() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // The register mask word is read before the effective address word,
    // but the memory operand keeps semantic position 1.
    const uint16_t movem[] = {0044252, 0x007F, 0x1234};  // MOVEM.W D0-D6, ($1234,A2)
    TASK_CHECK(decode16(dis, insn, text, sizeof text, movem, 3) == OK);
    TASK_CHECK(strcmp(text, "D0-D6, ($1234,A2)") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 1);
    const auto &m = insn.memoryOperands()[0];
    TASK_CHECK(m.operandIndex == 1);
    TASK_CHECK(m.kind == MemoryOperand::DISPLACEMENT && m.baseRegister == REG_A2);
    TASK_CHECK(m.baseDisp == 0x1234 && m.elementSize == 2);

    const uint16_t movem_idx[] = {0044262, 0x007F, 0xB812};  // MOVEM.W D0-D6, (18,A2,A3.L)
    TASK_CHECK(decode16(dis, insn, text, sizeof text, movem_idx, 3) == OK);
    TASK_CHECK(strcmp(text, "D0-D6, (18,A2,A3.L)") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 1);
    TASK_CHECK(insn.memoryOperands()[0].operandIndex == 1);
    TASK_CHECK(insn.memoryOperands()[0].kind == MemoryOperand::INDEXED);
    return 0;
}

int placement_pc_base_is_extension_word() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // 6-byte instruction: the PC base must be extension-word address
    // ORIGIN+2, not the instruction end ORIGIN+6.
    const uint16_t pcdsp[] = {0041772, 0x1232};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, pcdsp, 2) == OK);
    TASK_CHECK(insn.memoryOperands()[0].baseDisp == 0x1232);  // raw encoded value
    TASK_CHECK(strstr(text, "$00002234") != nullptr);         // 0x1000 + 2 + 0x1232

    // Same check at a different origin to rule out end-of-instruction math.
    Insn insn2(0x2000);
    TASK_CHECK(decode16(dis, insn2, text, sizeof text, pcdsp, 2, 0x2000) == OK);
    TASK_CHECK(insn2.memoryOperands()[0].baseDisp == 0x1232);
    TASK_CHECK(strstr(text, "$00003234") != nullptr);         // 0x2000 + 2 + 0x1232

    // Brief PC extension stores the raw disp8; text resolves at ORIGIN+2.
    const uint16_t briefpc[] = {0022073, 0x0234};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, briefpc, 2) == OK);
    TASK_CHECK(insn.memoryOperands()[0].baseDisp == 0x34);
    TASK_CHECK(strstr(text, "$00001036") != nullptr);         // 0x1000 + 2 + 0x34
    return 0;
}

// ---- lifetime group ------------------------------------------------------

int lifetime_clear_on_decode_start() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t memory_insn[] = {0022067, 0010426, 0x1234};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, memory_insn, 3) == OK);
    TASK_CHECK(insn.memoryOperandCount() == 1);

    // A register-only decode starts by dropping the previous model.
    const uint16_t reg_insn[] = {0x2401};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, reg_insn, 1) == OK);
    TASK_CHECK(strcmp(text, "D1, D2") == 0);
    TASK_CHECK(insn.memoryOperandCount() == 0);
    TASK_CHECK(insn.memoryOperands() == nullptr);
    return 0;
}

int lifetime_reset_clears() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t memory_insn[] = {0x2010};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, memory_insn, 1) == OK);
    TASK_CHECK(insn.memoryOperandCount() == 1);

    insn.reset(ORIGIN);
    TASK_CHECK(insn.memoryOperandCount() == 0);
    TASK_CHECK(insn.memoryOperands() == nullptr);

    // The opaque continuation state has its own lifetime and survives reset.
    struct Continuation {
        int marker;
    };
    insn.state<Continuation>().marker = 0x68020;
    insn.reset(ORIGIN);
    TASK_CHECK(insn.state<Continuation>().marker == 0x68020);
    TASK_CHECK(insn.memoryOperandCount() == 0);
    return 0;
}

int lifetime_truncation_cleanup() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    // Post-indexed full extension whose outer displacement word is missing.
    const uint16_t truncated[] = {0022067, 0010426};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, truncated, 2) == NO_MEMORY);
    TASK_CHECK(insn.length() == 4);            // consumed bytes are preserved
    TASK_CHECK(strstr(text, "([A7],D1.W") != nullptr);  // diagnostics preserved
    TASK_CHECK(insn.memoryOperandCount() == 0);
    TASK_CHECK(insn.memoryOperands() == nullptr);

    // The next, complete decode repopulates the model normally.
    const uint16_t good[] = {0x2010};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, good, 1) == OK);
    TASK_CHECK(insn.memoryOperandCount() == 1);
    TASK_CHECK(insn.memoryOperands()[0].kind == MemoryOperand::INDIRECT);
    return 0;
}

int lifetime_buffer_reuse_and_ownership() {
    DisMc68000 holder;
    auto &dis = *new68020(holder);
    Insn insn(ORIGIN);
    char text[160] = {0};

    const uint16_t first_words[] = {0027071, 0x1234, 0x5678};
    TASK_CHECK(decode16(dis, insn, text, sizeof text, first_words, 3) == OK);
    TASK_CHECK(insn.memoryOperandCount() == 1);
    // The backing ArrayMemory is already gone; copy values out and prove
    // they do not alias the output buffer.
    const auto saved = insn.memoryOperands()[0];
    memset(text, 'X', sizeof text - 1);
    TASK_CHECK(insn.memoryOperands()[0].baseDisp == 0x12345678);
    TASK_CHECK(sameOperand(saved, insn.memoryOperands()[0]));

    // Repeated decodes into the same Insn/buffer keep each result self-contained.
    const uint16_t second_words[] = {0022067, 0010422, 0x1234};
    for (int i = 0; i < 3; ++i) {
        TASK_CHECK(decode16(dis, insn, text, sizeof text, second_words, 3) == OK);
        TASK_CHECK(strcmp(text, "([A7,D1.W],$1234), D2") == 0);
        TASK_CHECK(insn.memoryOperandCount() == 1);
        TASK_CHECK(insn.memoryOperands()[0].kind == MemoryOperand::MEMORY_PREINDEXED);
        TASK_CHECK(insn.memoryOperands()[0].outerDisp == 0x1234);
    }

    // A second Insn decoding the same bytes gets an independent, equal model.
    Insn other(ORIGIN);
    char other_text[160] = {0};
    TASK_CHECK(decode16(dis, other, other_text, sizeof other_text, second_words, 3) == OK);
    TASK_CHECK(other.memoryOperandCount() == 1);
    TASK_CHECK(sameOperand(other.memoryOperands()[0], insn.memoryOperands()[0]));
    TASK_CHECK(other.memoryOperands() != insn.memoryOperands());
    return 0;
}

}  // namespace

const struct task_case task_cases[] = {
        {"structure", "indirect_modes", structure_indirect_modes},
        {"structure", "element_sizes", structure_element_sizes},
        {"structure", "displacement_and_absolute", structure_displacement_and_absolute},
        {"structure", "brief_extension", structure_brief_extension},
        {"structure", "pc_forms", structure_pc_forms},
        {"structure", "null_vs_zero_displacement", structure_null_vs_zero_displacement},
        {"structure", "full_extension_levels", structure_full_extension_levels},
        {"structure", "suppressed_components", structure_suppressed_components},
        {"structure", "register_immediate_no_entry",
                structure_register_and_immediate_have_no_entry},
        {"structure", "fpu_pmmu_excluded", structure_fpu_pmmu_excluded},
        {"structure", "display_options_invariant", structure_display_options_invariant},
        {"placement", "move_two_memory_operands", placement_move_two_memory_operands},
        {"placement", "movem_mask_before_extension", placement_movem_mask_before_extension},
        {"placement", "pc_base_is_extension_word", placement_pc_base_is_extension_word},
        {"lifetime", "clear_on_decode_start", lifetime_clear_on_decode_start},
        {"lifetime", "reset_clears", lifetime_reset_clears},
        {"lifetime", "truncation_cleanup", lifetime_truncation_cleanup},
        {"lifetime", "buffer_reuse_and_ownership", lifetime_buffer_reuse_and_ownership},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
