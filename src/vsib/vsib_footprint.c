/*
 * BeaEngine - VSIB per-lane memory footprint.
 *
 * Pure arithmetic over a decoded descriptor and a caller supplied register
 * snapshot. No target memory is read, no instruction is executed and no
 * host AVX support is required: vector images are byte arrays parsed
 * little-endian by hand.
 */

#include <string.h>
#include <beaengine/vsib.h>
#include "vsib_internal.h"

#define VSIB_LOW_CANONICAL_LIMIT  0x00007FFFFFFFFFFFull
#define VSIB_HIGH_CANONICAL_BASE  0xFFFF800000000000ull

static UInt32 vsib_read_u32(const UInt8* p)
{
   return (UInt32) p[0]
        | ((UInt32) p[1] << 8)
        | ((UInt32) p[2] << 16)
        | ((UInt32) p[3] << 24);
}

static UInt64 vsib_read_u64(const UInt8* p)
{
   return (UInt64) vsib_read_u32(p)
        | ((UInt64) vsib_read_u32(p + 4) << 32);
}

static Int64 vsib_sign_extend(UInt64 value, int widthBits)
{
   if (widthBits == 32) {
      UInt32 v = (UInt32) value;
      return (Int64) (Int32) v;
   }
   return (Int64) value;
}

static int vsib_is_canonical(UInt64 address)
{
   return address <= VSIB_LOW_CANONICAL_LIMIT
       || address >= VSIB_HIGH_CANONICAL_BASE;
}

static int vsib_validate_desc(const BeaVsibDesc* d)
{
   if (d->encoding != BEA_VSIB_ENCODING_VEX
       && d->encoding != BEA_VSIB_ENCODING_EVEX) {
      return 0;
   }
   if (d->operation != BEA_VSIB_GATHER
       && d->operation != BEA_VSIB_SCATTER) {
      return 0;
   }
   if (d->indexWidth != 32 && d->indexWidth != 64) {
      return 0;
   }
   if (d->dataWidth != 32 && d->dataWidth != 64) {
      return 0;
   }
   if (d->vectorBits != 128 && d->vectorBits != 256
       && d->vectorBits != 512) {
      return 0;
   }
   if (d->addressSize != 32 && d->addressSize != 64) {
      return 0;
   }
   if (d->scale != 1 && d->scale != 2 && d->scale != 4 && d->scale != 8) {
      return 0;
   }
   if (d->indexRegister >= 32) {
      return 0;
   }
   if (d->laneCount == 0 || d->laneCount > VSIB_MAX_LANES) {
      return 0;
   }
   {
      int widest = d->indexWidth > d->dataWidth ? d->indexWidth
                                                 : d->dataWidth;
      if (d->laneCount != (UInt16) (d->vectorBits / widest)) {
         return 0;
      }
   }
   if (d->hasBase) {
      if (d->baseRegister >= 16) {
         return 0;
      }
   }
   else if (d->baseRegister != BEA_VSIB_NO_REG) {
      return 0;
   }
   if (d->encoding == BEA_VSIB_ENCODING_EVEX) {
      if (d->maskKind != BEA_VSIB_MASK_K || d->maskRegister < 1
          || d->maskRegister > 7) {
         return 0;
      }
   }
   else {
      if (d->maskKind != BEA_VSIB_MASK_VEX_SIGN
          || d->maskRegister >= 32) {
         return 0;
      }
   }
   return 1;
}

/* Whether lane L is active under the descriptor's mask source. */
static int vsib_lane_active(const BeaVsibDesc* d,
                            const BeaVsibSnapshot* snap, int lane)
{
   if (d->encoding == BEA_VSIB_ENCODING_EVEX) {
      return (int) ((snap->k[d->maskRegister] >> lane) & 1u);
   }
   /* VEX: sign bit of the data-width mask element for this lane. */
   {
      const UInt8* maskVec = snap->vector[d->maskRegister];
      int byteOffset = lane * (d->dataWidth / 8);
      if (d->dataWidth == 32) {
         return (vsib_read_u32(maskVec + byteOffset) >> 31) & 1;
      }
      return (int) ((vsib_read_u64(maskVec + byteOffset) >> 63) & 1u);
   }
}

static UInt64 vsib_segment_base(const BeaVsibDesc* d,
                                const BeaVsibSnapshot* snap)
{
   /* Long mode: only FS/GS overrides contribute; every other segment
    * (including the default DS/SS tagging) is ignored. */
   if (d->segment == FSReg) {
      return snap->fsBase;
   }
   if (d->segment == GSReg) {
      return snap->gsBase;
   }
   return 0;
}

int __bea_callspec__ BeaVsibFootprint(const BeaVsibDesc* desc,
                                      const BeaVsibSnapshot* snapshot,
                                      BeaVsibRecord* records,
                                      UInt32 capacity,
                                      UInt32* recordCount)
{
   int lane;
   UInt32 count = 0;
   UInt64 segBase;
   int accessBytes;

   if (desc == NULL || snapshot == NULL || recordCount == NULL) {
      return BEA_VSIB_ERR_BAD_ARGUMENT;
   }
   if (capacity > 0 && records == NULL) {
      return BEA_VSIB_ERR_BAD_ARGUMENT;
   }
   if (!vsib_validate_desc(desc)) {
      return BEA_VSIB_ERR_BAD_ARGUMENT;
   }

   *recordCount = 0;
   accessBytes = desc->dataWidth / 8;
   segBase = vsib_segment_base(desc, snapshot);

   /* First pass: count active lanes so an undersized buffer is reported
    * without leaving a partial, misleading footprint behind. */
   for (lane = 0; lane < desc->laneCount; lane++) {
      if (vsib_lane_active(desc, snapshot, lane)) {
         count++;
      }
   }
   if (count > capacity) {
      return BEA_VSIB_ERR_NO_SPACE;
   }
   if (count == 0) {
      /* An all-zero mask is a valid, empty footprint. */
      return BEA_VSIB_OK;
   }

   for (lane = 0; lane < desc->laneCount; lane++) {
      UInt64 rawIndex;
      Int64 signedIndex;
      UInt64 effective;
      UInt64 linear;
      UInt64 tail;
      BeaVsibRecord* rec;
      const UInt8* indexVec;

      if (!vsib_lane_active(desc, snapshot, lane)) {
         continue;
      }

      indexVec = snapshot->vector[desc->indexRegister];
      if (desc->indexWidth == 32) {
         rawIndex = vsib_read_u32(indexVec + lane * 4);
      }
      else {
         rawIndex = vsib_read_u64(indexVec + lane * 8);
      }
      signedIndex = vsib_sign_extend(rawIndex, desc->indexWidth);

      /* base + sign-extended index * scale + displacement, wrapped mod
       * 2^64 before the address-size truncation. */
      effective = (desc->hasBase
                   ? snapshot->gpr[desc->baseRegister] : 0);
      effective = effective
                + (UInt64) signedIndex * (UInt64) desc->scale
                + (UInt64) desc->displacement;

      if (desc->addressSize == 32) {
         effective &= 0xFFFFFFFFull;
      }

      /* Segment base is applied after the address-size modulus and the
       * whole linear address is reduced mod 2^64. */
      linear = effective + segBase;

      rec = &records[*recordCount];
      (void) memset(rec, 0, sizeof(*rec));
      rec->lane = (UInt8) lane;
      rec->access = (desc->operation == BEA_VSIB_GATHER)
                        ? BEA_VSIB_ACCESS_READ : BEA_VSIB_ACCESS_WRITE;
      rec->accessBytes = (UInt8) accessBytes;
      rec->indexValue = signedIndex;

      /* The access covers [linear, linear + accessBytes - 1] as one
       * contiguous run: a 32-bit address size never re-wraps its tail. */
      tail = linear + (UInt64) (accessBytes - 1);
      if (tail < linear
          || !vsib_is_canonical(linear)
          || !vsib_is_canonical(tail)) {
         rec->status = BEA_VSIB_ADDRESS_INVALID;
         rec->address = 0;
      }
      else {
         rec->status = BEA_VSIB_ADDRESS_VALID;
         rec->address = linear;
      }
      (*recordCount)++;
   }

   return BEA_VSIB_OK;
}
