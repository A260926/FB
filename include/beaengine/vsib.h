/*
 * BeaEngine - VSIB gather/scatter description and memory footprint API
 *
 * This file is part of BeaEngine.
 *
 *    BeaEngine is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    BeaEngine is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public License
 *    along with BeaEngine.  If not, see <http://www.gnu.org/licenses/>. */

#ifndef _BEA_ENGINE_VSIB_H_
#define _BEA_ENGINE_VSIB_H_

#include <beaengine/BeaEngine.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 *  Status codes returned by the VSIB entry points.
 *  Success is always BEA_VSIB_OK (0); every failure code is negative
 *  and is never hidden in formatted text.
 * ==================================================================== */
#define BEA_VSIB_OK                 0
#define BEA_VSIB_ERR_TRUNCATED     -1  /* SecurityBlock ends mid-instruction */
#define BEA_VSIB_ERR_NOT_VSIB      -2  /* decoded instruction has no VSIB memory */
#define BEA_VSIB_ERR_UNSUPPORTED   -3  /* unsupported instruction or mode */
#define BEA_VSIB_ERR_BAD_ENCODING  -4  /* illegal register/mask/ModRM combination */
#define BEA_VSIB_ERR_BAD_ARGUMENT  -5  /* NULL/inconsistent caller argument */
#define BEA_VSIB_ERR_NO_SPACE      -6  /* record array too small */

/* ====================================================================
 *  Encoding class stored in BeaVsibDesc.encoding
 * ==================================================================== */
#define BEA_VSIB_ENCODING_VEX      1
#define BEA_VSIB_ENCODING_EVEX     2

/* ====================================================================
 *  Direction stored in BeaVsibDesc.operation
 * ==================================================================== */
#define BEA_VSIB_GATHER            1
#define BEA_VSIB_SCATTER           2

/* ====================================================================
 *  Mask source stored in BeaVsibDesc.maskKind
 * ==================================================================== */
#define BEA_VSIB_MASK_NONE         0
#define BEA_VSIB_MASK_VEX_SIGN     1  /* sign bits of the VEX mask vector */
#define BEA_VSIB_MASK_K            2  /* EVEX opmask register kN */

/* ====================================================================
 *  Per-record address status stored in BeaVsibRecord.status
 * ==================================================================== */
#define BEA_VSIB_ADDRESS_VALID     0
#define BEA_VSIB_ADDRESS_INVALID   1

/* ====================================================================
 *  Read/write attribute stored in BeaVsibRecord.access
 * ==================================================================== */
#define BEA_VSIB_ACCESS_READ       1
#define BEA_VSIB_ACCESS_WRITE      2

/* Register number meaning "no register" (e.g. a base-less SIB). */
#define BEA_VSIB_NO_REG            0xFF

/* Largest possible lane count (512-bit vector over 32-bit elements). */
#define BEA_VSIB_MAX_LANES         16

/*
 * Static description of one decoded VSIB gather/scatter instruction.
 *
 * indexWidth and dataWidth are independent. The effective lane count is
 * vectorBits / max(indexWidth, dataWidth) and is stored precomputed in
 * laneCount; it is never derived from a printed memory size or from the
 * destination register width.
 *
 * displacement already holds the final effective displacement: an EVEX
 * compressed disp8 has been multiplied by the data element size, while a
 * disp32 is used verbatim.
 */
#pragma pack(1)
typedef struct {
   UInt8  encoding;           /* BEA_VSIB_ENCODING_VEX / _EVEX */
   UInt8  operation;          /* BEA_VSIB_GATHER / _SCATTER */
   UInt8  addressSize;        /* effective address width: 32 or 64 */
   UInt8  segment;            /* FSReg / GSReg, or 0 when ignored */
   UInt8  maskKind;           /* BEA_VSIB_MASK_* */
   UInt8  maskRegister;       /* k register (EVEX) or mask vector (VEX) */
   UInt8  indexWidth;         /* bits per index element: 32 or 64 */
   UInt8  dataWidth;          /* bits per data element: 32 or 64 */
   UInt8  scale;              /* 1, 2, 4 or 8 */
   UInt8  hasBase;            /* 0 for a base-less SIB, 1 otherwise */
   UInt8  baseRegister;       /* GPR number 0..15; BEA_VSIB_NO_REG if !hasBase */
   UInt8  indexRegister;      /* vector register number 0..31 */
   UInt16 vectorBits;         /* 128, 256 or 512 */
   UInt16 laneCount;          /* effective number of lanes */
   UInt16 instructionLength;  /* full encoded instruction length in bytes */
   Int64  displacement;       /* final effective, sign-extended displacement */
} BeaVsibDesc;
#pragma pack()

/*
 * Caller supplied register snapshot.
 *  - gpr[0..15]    : RAX..R15 full 64-bit values
 *  - vector[r][b]  : complete 64-byte image of vector register r (zmm),
 *                    elements interpreted little-endian
 *  - k[0..7]       : opmask registers
 *  - fsBase/gsBase : segment bases used in long mode
 * The structure is read only; no memory pointed at by an address is read.
 */
#pragma pack(1)
typedef struct {
   UInt64 gpr[16];
   UInt8  vector[32][64];
   UInt64 k[8];
   UInt64 fsBase;
   UInt64 gsBase;
} BeaVsibSnapshot;
#pragma pack()

/*
 * One active-lane memory access. Records are emitted in ascending lane
 * order; lanes sharing an address are kept separate and overlapping
 * records are never merged.
 */
#pragma pack(1)
typedef struct {
   UInt8  lane;               /* original lane number */
   UInt8  access;             /* BEA_VSIB_ACCESS_READ / _WRITE */
   UInt8  status;             /* BEA_VSIB_ADDRESS_VALID / _INVALID */
   UInt8  accessBytes;        /* 4 or 8 contiguous bytes per access */
   Int64  indexValue;         /* sign-extended index used for this lane */
   UInt64 address;            /* linear start address, 0 when invalid */
} BeaVsibRecord;
#pragma pack()

/*
 * Decode the VSIB instruction described by pMyDisasm (same input rules as
 * Disasm: EIP/SecurityBlock/Archi/VirtualAddr/Options). On success returns
 * BEA_VSIB_OK and fills *desc. On any failure *desc is cleared before
 * returning and a negative BEA_VSIB_ERR_* code is returned; the ordinary
 * DISASM/Disasm ABI and behaviour are unaffected.
 */
BEA_API int __bea_callspec__ BeaVsibDecode(PDISASM pMyDisasm, BeaVsibDesc* desc);

/*
 * Compute the per-lane memory footprint of a previously decoded VSIB
 * instruction from a register snapshot. Only active lanes produce records.
 * Masked-off lanes (including an all-zero mask, which yields an empty
 * footprint) simply generate nothing. Invalid active lanes (non-canonical
 * access range or uint64 tail overflow) are kept as
 * BEA_VSIB_ADDRESS_INVALID records while the other lanes are still
 * reported. Returns BEA_VSIB_OK on success or a negative status code.
 */
BEA_API int __bea_callspec__ BeaVsibFootprint(const BeaVsibDesc* desc,
                                              const BeaVsibSnapshot* snapshot,
                                              BeaVsibRecord* records,
                                              UInt32 capacity,
                                              UInt32* recordCount);

#ifdef __cplusplus
}
#endif

#endif /* _BEA_ENGINE_VSIB_H_ */
