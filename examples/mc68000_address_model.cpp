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

// C++14 calling example for the structured MC68000/MC68020 memory operand
// model exposed by Insn::memoryOperands(). It only decodes synthetic bytes;
// it never reads target memory or registers and never executes anything.
//
// Build against the library sources, for example:
//   c++ -std=c++14 -I. -Isrc examples/mc68000_address_model.cpp \
//       $(find src -name '*.cpp') -o mc68000_address_model
//
// Field units (see src/memory_operand.h for the full contract):
//   elementSize   access width in bytes (1, 2 or 4)
//   indexSize     index register width in bytes (2/4); 0 when absent
//   indexScale    dimensionless multiplier 1/2/4/8
//   baseDisp,     signed displacements in byte address units; a null
//   outerDisp     encoded field leaves the value 0 and sets the matching
//                 DisplacementSize to DISP_NULL, distinct from an encoded
//                 zero word/long
//   baseRegister/ architecture RegName identity, or -1 when suppressed or
//   indexRegister absent
//   absoluteShort when true, baseDisp is the sign-extended 32-bit address

#include <cstdint>
#include <cstdio>

#include "array_memory.h"
#include "dis_mc68000.h"
#include "memory_operand.h"

using namespace libasm;
using namespace libasm::mc68000;

namespace {

const char *kindText(MemoryOperand::Kind kind) {
    switch (kind) {
    case MemoryOperand::ABSOLUTE:
        return "absolute";
    case MemoryOperand::INDIRECT:
        return "(An)";
    case MemoryOperand::POSTINCREMENT:
        return "(An)+";
    case MemoryOperand::PREDECREMENT:
        return "-(An)";
    case MemoryOperand::DISPLACEMENT:
        return "(d16,An)";
    case MemoryOperand::PC_DISPLACEMENT:
        return "(d16,PC)";
    case MemoryOperand::INDEXED:
        return "B+bd+I";
    case MemoryOperand::PC_INDEXED:
        return "PC+bd+I";
    case MemoryOperand::MEMORY_PREINDEXED:
        return "Mem32[B+bd+I]+od";
    case MemoryOperand::MEMORY_POSTINDEXED:
        return "Mem32[B+bd]+I+od";
    }
    return "?";
}

void printOperand(const MemoryOperand &m) {
    std::printf("  semantic operand #%u, %u-byte access, %s\n", m.operandIndex, m.elementSize,
            kindText(m.kind));
    std::printf("    base reg=%d suppressed=%d pcBase=%d pre=%d post=%d\n", m.baseRegister,
            m.baseSuppressed, m.pcBase, m.predecrement, m.postincrement);
    std::printf("    index reg=%d suppressed=%d size=%u bytes scale=%u\n", m.indexRegister,
            m.indexSuppressed, m.indexSize, m.indexScale);
    std::printf("    base disp=%d (encoded size %d), outer disp=%d (encoded size %d)\n",
            m.baseDisp, m.baseDispSize, m.outerDisp, m.outerDispSize);
    if (m.absoluteShort)
        std::printf("    absolute short: baseDisp is the sign-extended 32-bit address\n");
}

}  // namespace

int main() {
    DisMc68000 holder;
    Disassembler &disassembler = holder;
    disassembler.setCpu("68020");

    // MOVE.L ([18,A0,D1.L*2],$0034), D0 -- full extension, pre-indexed.
    const uint16_t words[] = {0x2030, 0x1B22, 0x0012, 0x0034};
    const ArrayMemory memory(0x1000, words, sizeof(words), ENDIAN_BIG, ADDRESS_BYTE);
    auto cursor = memory.iterator();

    Insn insn(cursor.address());
    char operands[128] = {0};
    const Error error = disassembler.decode(cursor, insn, operands, sizeof(operands));
    if (error != OK) {
        std::printf("decode failed: %d\n", error);
        return 1;
    }

    std::printf("%s %s (%u bytes), %zu memory operand(s)\n", insn.name(), operands, insn.length(),
            insn.memoryOperandCount());
    for (size_t i = 0; i < insn.memoryOperandCount(); ++i)
        printOperand(insn.memoryOperands()[i]);
    return 0;
}
