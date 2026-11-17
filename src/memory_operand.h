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

#ifndef __LIBASM_MEMORY_OPERAND_H__
#define __LIBASM_MEMORY_OPERAND_H__

#include <stddef.h>
#include <stdint.h>

namespace libasm {

/**
 * Structured description of one memory-referencing effective address,
 * produced while an instruction is disassembled.
 *
 * The model only describes the addressing arithmetic encoded in the
 * instruction; it never reads target memory, registers or the program
 * counter, and it never evaluates the effective address. The address
 * hierarchy is expressed as:
 *
 *   no indirection:  B + bd + I
 *   pre-indexed:     Mem32[B + bd + I] + od
 *   post-indexed:    Mem32[B + bd] + I + od
 *
 * where B is the base register, bd the base displacement, I the scaled
 * index register and od the outer displacement. Suppressed components
 * are simply absent from the expression; Mem32 is never invoked.
 *
 * All values are owned by the Insn that produced them and stay valid
 * until that Insn is reset or another instruction is decoded into it.
 * Register identities use the architecture's own RegName enum, so a
 * MemoryOperand header stays free of per-architecture definitions.
 *
 * Field units and encodings:
 *   operandIndex    semantic operand position, zero-based count (no unit).
 *   elementSize     memory access width in bytes (1, 2 or 4).
 *   baseRegister/   architecture RegName value, or -1; identity, not a
 *   indexRegister   number of bytes.
 *   indexSize       index register width in bytes (2 or 4); 0 if absent.
 *   indexScale      dimensionless multiplier 1/2/4/8 applied to the index.
 *   baseDisp/outerDisp
 *                   signed displacement values, in address units (bytes on
 *                   the byte-addressable 68k); DISP_NULL keeps the value 0.
 *   baseDispSize/   width of the field as encoded in the instruction, not
 *   outerDispSize   its numeric magnitude: null, byte, word or long.
 *   absoluteShort   flag; when set baseDisp already holds the sign-extended
 *                   32-bit address rather than the raw 16-bit field.
 *   pcBase          flag; addresses are taken relative to the extension
 *                   word location, not the end of the instruction.
 */
struct MemoryOperand {
    /** How the effective address reaches memory. */
    enum Kind : uint8_t {
        /** No base register: (bd) and (bd).W/(bd).L style absolute addresses. */
        ABSOLUTE = 0,
        /** (An): address register indirect. */
        INDIRECT = 1,
        /** (An)+: postincrement. */
        POSTINCREMENT = 2,
        /** -(An): predecrement. */
        PREDECREMENT = 3,
        /** (d16,An): address register indirect with displacement. */
        DISPLACEMENT = 4,
        /** (d16,PC): program counter indirect with displacement. */
        PC_DISPLACEMENT = 5,
        /**
         * (d8,An,Xn) or a full-extension word without memory
         * indirection: B + bd + I.
         */
        INDEXED = 6,
        /**
         * (d8,PC,Xn) or a PC-based full-extension word without memory
         * indirection: PC + bd + I.
         */
        PC_INDEXED = 7,
        /** Full extension, pre-indexed memory indirection: Mem32[B+bd+I]+od. */
        MEMORY_PREINDEXED = 8,
        /** Full extension, post-indexed memory indirection: Mem32[B+bd]+I+od. */
        MEMORY_POSTINDEXED = 9,
    };

    /** Encoded width of a displacement field. */
    enum DisplacementSize : uint8_t {
        /** No displacement word follows the extension/effective address. */
        DISP_NULL = 0,
        /** A signed 8-bit displacement embedded in a brief extension word. */
        DISP_BYTE = 1,
        /** A signed 16-bit displacement word. */
        DISP_WORD = 2,
        /** A signed 32-bit displacement long word. */
        DISP_LONG = 3,
    };

    /** Width of an index register, in bytes. */
    enum IndexSize : uint8_t {
        INDEX_NONE = 0,
        INDEX_WORD = 2,
        INDEX_LONG = 4,
    };

    /**
     * Semantic operand position, 0-based in the order operands are
     * printed (source, destination, ...). Register lists and other
     * non-memory operands keep their position but produce no entry.
     */
    uint8_t operandIndex;

    /** Access size in bytes: 1, 2 or 4. */
    uint8_t elementSize;

    /** Addressing structure of this operand. */
    Kind kind;

    /**
     * Base register identity (architecture RegName), or -1 when the
     * base is suppressed or the addressing has no base register.
     */
    int8_t baseRegister;
    /** True when the base register is suppressed by the encoding. */
    bool baseSuppressed;

    /**
     * Index register identity (architecture RegName), or -1 when no
     * index register is part of the effective address.
     */
    int8_t indexRegister;
    /** True when the index register is suppressed by the encoding. */
    bool indexSuppressed;
    /** Index register width in bytes; INDEX_NONE when there is no index. */
    IndexSize indexSize;
    /** Index scale factor: 1, 2, 4 or 8; 1 when the encoding omits it. */
    uint8_t indexScale;

    /**
     * True when the base is the program counter, including a PC that
     * the full extension word suppresses. A suppressed PC still fetches
     * its displacement from program space, so this must not be confused
     * with an ordinary base-less absolute operand.
     */
    bool pcBase;
    /** True for predecrement (-(An)); false otherwise. */
    bool predecrement;
    /** True for postincrement ((An)+); false otherwise. */
    bool postincrement;

    /** Encoded width of the base displacement field. */
    DisplacementSize baseDispSize;
    /** Base displacement as a signed value; 0 when baseDispSize is DISP_NULL. */
    int32_t baseDisp;

    /** Encoded width of the outer displacement field (full extension). */
    DisplacementSize outerDispSize;
    /** Outer displacement as a signed value; 0 when outerDispSize is DISP_NULL. */
    int32_t outerDisp;

    /**
     * True when the operand encodes an absolute short address. The
     * 16-bit value is then sign extended into baseDisp, which holds the
     * resulting 32-bit address.
     */
    bool absoluteShort;

    MemoryOperand()
        : operandIndex(0),
          elementSize(0),
          kind(ABSOLUTE),
          baseRegister(-1),
          baseSuppressed(false),
          indexRegister(-1),
          indexSuppressed(false),
          indexSize(INDEX_NONE),
          indexScale(1),
          pcBase(false),
          predecrement(false),
          postincrement(false),
          baseDispSize(DISP_NULL),
          baseDisp(0),
          outerDispSize(DISP_NULL),
          outerDisp(0),
          absoluteShort(false) {}
};

}  // namespace libasm

#endif  // __LIBASM_MEMORY_OPERAND_H__

// Local Variables:
// mode: c++
// c-basic-offset: 4
// tab-width: 4
// End:
// vim: set ft=cpp et ts=4 sw=4:
