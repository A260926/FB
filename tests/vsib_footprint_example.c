/*
 * BeaEngine VSIB API - minimal calling example.
 *
 * Decodes one EVEX VPGATHERDD, feeds in a register snapshot and prints the
 * per-lane read footprint. The example is self contained and builds on its
 * own, for example:
 *
 *   cc -std=c11 -Iinclude src/BeaEngine.c src/vsib/vsib_decode.c \
 *      src/vsib/vsib_footprint.c tests/vsib_footprint_example.c -o vsib_ex
 */

#include <beaengine/BeaEngine.h>
#include <beaengine/vsib.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
   /* vpgatherdd zmm1 {k1}, dword ptr [rax+zmm3*4] */
   const unsigned char code[] = {
      0x62, 0xf2, 0x7d, 0x49, 0x90, 0x0c, 0x98
   };
   DISASM d;
   BeaVsibDesc desc;
   BeaVsibSnapshot snap;
   BeaVsibRecord records[BEA_VSIB_MAX_LANES];
   UInt32 count = 0;
   int status;
   UInt32 lane;

   (void) memset(&d, 0, sizeof(d));
   d.EIP = (UIntPtr) code;
   d.SecurityBlock = (UInt32) sizeof(code);
   d.Archi = 64;
   d.VirtualAddr = 0x1000;
   d.Options = ShowEVEXMasking;

   status = BeaVsibDecode(&d, &desc);
   if (status != BEA_VSIB_OK) {
      printf("decode failed: %d\n", status);
      return 1;
   }
   printf("%s : %u lanes, index %u-bit, data %u-bit\n",
          d.CompleteInstr, desc.laneCount,
          desc.indexWidth, desc.dataWidth);

   (void) memset(&snap, 0, sizeof(snap));
   snap.gpr[0] = 0x10000;                 /* rax */
   for (lane = 0; lane < desc.laneCount; lane++) {
      snap.vector[3][lane * 4] = (UInt8) lane;   /* zmm3 dword indices */
   }
   snap.k[1] = 0x5555u;                   /* every even lane active */

   status = BeaVsibFootprint(&desc, &snap, records,
                             BEA_VSIB_MAX_LANES, &count);
   if (status != BEA_VSIB_OK) {
      printf("footprint failed: %d\n", status);
      return 1;
   }

   printf("active records: %u\n", count);
   for (lane = 0; lane < count; lane++) {
      const BeaVsibRecord* r = &records[lane];
      if (r->status == BEA_VSIB_ADDRESS_VALID) {
         printf("  lane %u: %s 0x%016llx (%u bytes), index %lld\n",
                r->lane,
                r->access == BEA_VSIB_ACCESS_READ ? "read " : "write",
                (unsigned long long) r->address, r->accessBytes,
                (long long) r->indexValue);
      }
      else {
         printf("  lane %u: invalid address, index %lld\n",
                r->lane, (long long) r->indexValue);
      }
   }
   return 0;
}
