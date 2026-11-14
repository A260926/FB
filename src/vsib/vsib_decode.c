/*
 * BeaEngine - VSIB decode entry point.
 *
 * BeaVsibDecode runs the ordinary Disasm() pipeline unchanged, so every
 * value below is produced by the existing opcode / prefix / ModRM / SIB
 * paths. The descriptor is then projected from the engine state; no
 * mnemonic string is parsed and no parallel instruction table is kept.
 */

#include <string.h>
#include <beaengine/vsib.h>
#include "vsib_internal.h"

/* Locate the (single) VSIB memory operand filled by the decoder. */
static OPTYPE* vsib_memory_operand(PDISASM d)
{
   OPTYPE* candidates[3];
   int i;
   candidates[0] = &d->Operand1;
   candidates[1] = &d->Operand2;
   candidates[2] = &d->Operand3;
   for (i = 0; i < 3; i++) {
      if ((candidates[i]->OpType & 0x000f0000) == MEMORY_TYPE) {
         return candidates[i];
      }
   }
   return NULL;
}

/* REGS[n] is the single bit 1<<n; recover n in strictly standard C. */
static int vsib_reg_number(Int64 bit)
{
   int n = 0;
   UInt64 v = (UInt64) bit;
   if (v == 0) {
      return -1;
   }
   while ((v & 1u) == 0) {
      v >>= 1;
      n++;
   }
   return n;
}

static int vsib_vector_bits(Int32 vsibClass)
{
   if (vsibClass == SSE_REG) {
      return 128;
   }
   if (vsibClass == AVX_REG) {
      return 256;
   }
   if (vsibClass == AVX512_REG) {
      return 512;
   }
   return 0;
}

int __bea_callspec__ BeaVsibDecode(PDISASM pMyDisasm, BeaVsibDesc* desc)
{
   int len;
   int isEvex;
   int opcodeByte;
   int vectorBits;
   int indexWidth;
   int dataWidth;
   int laneCount;
   int scale;
   int indexRegister;
   int hasBase;
   int baseRegister;
   OPTYPE* memOp;
   const InternalDatas* gv;

   if (desc == NULL || pMyDisasm == NULL) {
      return BEA_VSIB_ERR_BAD_ARGUMENT;
   }
   (void) memset(desc, 0, sizeof(*desc));

   len = Disasm(pMyDisasm);
   gv = &pMyDisasm->Reserved_;

   if (len == OUT_OF_BLOCK) {
      return BEA_VSIB_ERR_TRUNCATED;
   }
   if (len <= 0) {
      return (gv->VSIB_ != 0) ? BEA_VSIB_ERR_BAD_ENCODING
                              : BEA_VSIB_ERR_NOT_VSIB;
   }
   if (pMyDisasm->Error == UD_) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }
   if (gv->Architecture != 64) {
      return BEA_VSIB_ERR_UNSUPPORTED;
   }
   if (gv->VSIB_ == 0) {
      return BEA_VSIB_ERR_NOT_VSIB;
   }

   memOp = vsib_memory_operand(pMyDisasm);
   if (memOp == NULL || gv->RM_ != 4) {
      /* VSIB addressing always goes through a SIB byte. */
      return BEA_VSIB_ERR_NOT_VSIB;
   }

   isEvex = (gv->EVEX.state == InUsePrefix);
   opcodeByte = pMyDisasm->Instruction.Opcode & 0xff;

   vectorBits = vsib_vector_bits(gv->VSIB_);
   if (vectorBits == 0) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }

   /* Index element width comes straight from the W bit. */
   if (isEvex) {
      indexWidth = gv->EVEX.W ? 64 : 32;
   }
   else {
      indexWidth = gv->REX.W_ ? 64 : 32;
   }

   /* Data element width is the size the decoder attached to the memory
    * operand (Arg2dword/Arg2qword decorations through ArgsSize). */
   dataWidth = memOp->OpSize;
   if (dataWidth != 32 && dataWidth != 64) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }

   if (indexWidth != 32 && indexWidth != 64) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }

   laneCount = vectorBits / (indexWidth > dataWidth ? indexWidth : dataWidth);
   if (laneCount <= 0 || laneCount > VSIB_MAX_LANES) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }

   /* GV.AddressSize is restored to the mode default by the 0x67 prefix
    * handler once the recursive decode unwinds, so the surviving signal is
    * Prefix.AddressSize: in 64-bit mode a present 0x67 means 32-bit
    * effective addresses and its absence means 64-bit. */
   {
      int effectiveAddressSize =
         (pMyDisasm->Prefix.AddressSize == InUsePrefix) ? 32 : 64;
      if (effectiveAddressSize != 32 && effectiveAddressSize != 64) {
         return BEA_VSIB_ERR_UNSUPPORTED;
      }
      desc->addressSize = (UInt8) effectiveAddressSize;
   }

   /* SIB scale (SCALE_ is the raw 2-bit field). */
   if (gv->SCALE_ < 0 || gv->SCALE_ > 3) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }
   scale = 1 << gv->SCALE_;

   /* VSIB vector index: the SIB index field always names a vector
    * register here (index 4 is xmm4/ymm4/zmm4, never "no index"). */
   if (isEvex) {
      if (gv->EVEX.X == 0) {
         indexRegister = gv->EVEX.V == 0 ? gv->INDEX_ : gv->INDEX_ + 16;
      }
      else {
         indexRegister = gv->EVEX.V == 0 ? gv->INDEX_ + 8
                                         : gv->INDEX_ + 8 + 16;
      }
   }
   else {
      if (gv->REX.X_ == 0) {
         indexRegister = gv->INDEX_;
      }
      else {
         indexRegister = gv->INDEX_ + 8;
      }
   }
   if (indexRegister < 0 || indexRegister > 31) {
      return BEA_VSIB_ERR_BAD_ENCODING;
   }

   /* Base: mod==0 with SIB base 5 is the base-less disp32 form and must
    * not be reported as rbp/r13. */
   if (gv->MOD_ == 0 && gv->BASE_ == 5) {
      hasBase = 0;
      baseRegister = BEA_VSIB_NO_REG;
   }
   else {
      hasBase = 1;
      baseRegister = gv->REX.B_ ? gv->BASE_ + 8 : gv->BASE_;
      if (baseRegister < 0 || baseRegister > 15) {
         return BEA_VSIB_ERR_BAD_ENCODING;
      }
   }

   /* Effective displacement. A raw EVEX disp8 is compressed once by the
    * data element byte count (the TUPLE1_SCALAR N factor); disp32 and the
    * base-less disp32 are used verbatim and are never scaled again. */
   {
      Int64 raw = memOp->Memory.Displacement;
      if (isEvex && gv->MOD_ == 1) {
         raw = raw * (dataWidth / 8);
      }
      desc->displacement = raw;
   }

   /* Mask source. */
   if (isEvex) {
      if (gv->EVEX.aaa == 0) {
         /* gather/scatter require k1..k7; k0 is #UD. */
         return BEA_VSIB_ERR_BAD_ENCODING;
      }
      desc->maskKind = BEA_VSIB_MASK_K;
      desc->maskRegister = (UInt8) gv->EVEX.aaa;
   }
   else {
      int maskRegister = vsib_reg_number(pMyDisasm->Operand3.Registers.xmm
                                         | pMyDisasm->Operand3.Registers.ymm
                                         | pMyDisasm->Operand3.Registers.zmm);
      if (maskRegister < 0) {
         return BEA_VSIB_ERR_BAD_ENCODING;
      }
      desc->maskKind = BEA_VSIB_MASK_VEX_SIGN;
      desc->maskRegister = (UInt8) maskRegister;
   }

   switch (opcodeByte) {
      case VSIB_OP_GATHER_DD_DQ:
      case VSIB_OP_GATHER_QD_QQ:
         desc->operation = BEA_VSIB_GATHER;
         break;
      case VSIB_OP_SCATTER_DD_DQ:
      case VSIB_OP_SCATTER_QD_QQ:
         desc->operation = BEA_VSIB_SCATTER;
         if (!isEvex) {
            return BEA_VSIB_ERR_BAD_ENCODING;
         }
         /* EVEX.z=1 on a scatter is #UD: scatters only support merging. */
         if (gv->EVEX.z == 1) {
            return BEA_VSIB_ERR_BAD_ENCODING;
         }
         break;
      default:
         return BEA_VSIB_ERR_UNSUPPORTED;
   }

   desc->encoding = isEvex ? BEA_VSIB_ENCODING_EVEX : BEA_VSIB_ENCODING_VEX;
   desc->segment = (UInt8) memOp->SegmentReg;
   desc->indexWidth = (UInt8) indexWidth;
   desc->dataWidth = (UInt8) dataWidth;
   desc->scale = (UInt8) scale;
   desc->hasBase = (UInt8) hasBase;
   desc->baseRegister = (UInt8) baseRegister;
   desc->indexRegister = (UInt8) indexRegister;
   desc->vectorBits = (UInt16) vectorBits;
   desc->laneCount = (UInt16) laneCount;
   desc->instructionLength = (UInt16) len;

   return BEA_VSIB_OK;
}
