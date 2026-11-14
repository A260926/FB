/*
 * BeaEngine - VSIB gather/scatter footprint test cases.
 *
 * Every instruction below is hand assembled and every expected address is
 * held in an independent table of literals: the production address formula
 * is never reused to compute the expected results. The three groups
 * (encoding, lanes, addresses) each print their own PASS line and the
 * whole program finishes with VSIB_FOOTPRINT_OK only when all pass.
 */

#include <beaengine/BeaEngine.h>
#include <beaengine/vsib.h>

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                          \
   do {                                                           \
      if (!(cond)) {                                              \
         g_failures++;                                            \
         printf("FAIL %s:%d: ", __FILE__, __LINE__);              \
         printf(__VA_ARGS__);                                     \
         printf("\n");                                            \
      }                                                           \
   } while (0)

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void put_u32(UInt8* p, UInt32 v)
{
   p[0] = (UInt8) v;
   p[1] = (UInt8) (v >> 8);
   p[2] = (UInt8) (v >> 16);
   p[3] = (UInt8) (v >> 24);
}

static void put_u64(UInt8* p, UInt64 v)
{
   put_u32(p, (UInt32) v);
   put_u32(p + 4, (UInt32) (v >> 32));
}

static int decode(const unsigned char* bytes, UInt32 n, BeaVsibDesc* desc)
{
   DISASM d;
   (void) memset(&d, 0, sizeof(d));
   d.EIP = (UIntPtr) bytes;
   d.SecurityBlock = n;
   d.Archi = 64;
   d.VirtualAddr = 0x1000;
   d.Options = ShowEVEXMasking;
   return BeaVsibDecode(&d, desc);
}

static int footprint(const BeaVsibDesc* d, const BeaVsibSnapshot* s,
                     BeaVsibRecord* r, UInt32 cap, UInt32* count)
{
   return BeaVsibFootprint(d, s, r, cap, count);
}

/* Hand assembled instruction bytes. */
static const unsigned char B_VEX_DD128[] = {0xc4,0xe2,0x69,0x90,0x0c,0x98};
static const unsigned char B_VEX_QQ256[] = {0xc4,0xe2,0xed,0x91,0x0c,0x98};
static const unsigned char B_VEX_QD256[]  = {0xc4,0xe2,0x6d,0x91,0x0c,0x98};
static const unsigned char B_EVEX_DD512_DISP8[] =
   {0x62,0xf2,0x7d,0x49,0x90,0x4c,0x98,0x02};
static const unsigned char B_EVEX_DD128_REF[] =
   {0x62,0xf2,0x7d,0x09,0x90,0x0c,0x98};
static const unsigned char B_EVEX_QQ512[] =
   {0x62,0xf2,0xfd,0x49,0x91,0x0c,0x98};
static const unsigned char B_EVEX_DD256[] = {0x62,0xf2,0x7d,0x29,0x90,0x0c,0x98};
static const unsigned char B_EVEX_DQ128[] = {0x62,0xf2,0xfd,0x09,0x90,0x0c,0x98};
static const unsigned char B_EVEX_QQ128[] = {0x62,0xf2,0xfd,0x09,0x91,0x0c,0x98};
static const unsigned char B_EVEX_QD128_SCATTER[] =
   {0x62,0xf2,0x7d,0x09,0xa1,0x0c,0x98};
static const unsigned char B_EVEX_DD512_IDX31[] =
   {0x62,0xb2,0x7d,0x41,0x90,0x0c,0xb8};
static const unsigned char B_EVEX_BASELESS[] =
   {0x62,0xf2,0x7d,0x49,0x90,0x0c,0x9d,0x78,0x56,0x34,0x12};
static const unsigned char B_VEX_FS_ADDR32[] =
   {0x64,0x67,0xc4,0xe2,0x6d,0x90,0x0c,0x98};
static const unsigned char B_EVEX_DISP8_NEG[] =
   {0x62,0xf2,0x7d,0x49,0x90,0x4c,0x98,0xfe};
static const unsigned char B_EVEX_DISP32_NEG[] =
   {0x62,0xf2,0x7d,0x49,0x90,0x8c,0x98,0xf8,0xff,0xff,0xff};
static const unsigned char B_EVEX_DISP8_POS[] =
   {0x62,0xf2,0x7d,0x49,0x90,0x4c,0x98,0x02};
static const unsigned char B_EVEX_DISP32_POS[] =
   {0x62,0xf2,0x7d,0x49,0x90,0x8c,0x98,0x08,0x00,0x00,0x00};
static const unsigned char B_TRUNCATED[] = {0x62,0xf2,0x7d,0x49,0x90,0x4c};
static const unsigned char B_K0[] = {0x62,0xf2,0x7d,0x48,0x90,0x0c,0x98};
static const unsigned char B_REG_FORM[] = {0x62,0xf2,0x7d,0x49,0x90,0xcc};
static const unsigned char B_MOV[] = {0x48,0x8b,0x05,0x04,0,0,0};

/* ------------------------------------------------------------------ */
/* group 1: descriptor fields straight from the shared decode paths    */
/* ------------------------------------------------------------------ */

static void test_encoding(void)
{
   BeaVsibDesc x;
   int st;

   st = decode(B_VEX_DD128, (UInt32) sizeof(B_VEX_DD128), &x);
   CHECK(st == BEA_VSIB_OK, "VEX DD128 status %d", st);
   CHECK(x.encoding == BEA_VSIB_ENCODING_VEX, "VEX encoding");
   CHECK(x.operation == BEA_VSIB_GATHER, "gather op");
   CHECK(x.vectorBits == 128, "DD128 vector bits %u", x.vectorBits);
   CHECK(x.indexWidth == 32 && x.dataWidth == 32, "DD widths");
   CHECK(x.laneCount == 4, "DD128 lanes %u", x.laneCount);
   CHECK(x.instructionLength == 6, "DD128 length %u", x.instructionLength);
   CHECK(x.scale == 4, "scale %u", x.scale);
   CHECK(x.hasBase == 1 && x.baseRegister == 0, "rax base");
   CHECK(x.indexRegister == 3, "xmm3 index");
   CHECK(x.addressSize == 64, "default addr size");
   CHECK(x.maskKind == BEA_VSIB_MASK_VEX_SIGN && x.maskRegister == 2,
         "VEX sign mask k%u", x.maskRegister);

   st = decode(B_VEX_QQ256, (UInt32) sizeof(B_VEX_QQ256), &x);
   CHECK(st == BEA_VSIB_OK, "VEX QQ256 status %d", st);
   CHECK(x.vectorBits == 256 && x.indexWidth == 64 && x.dataWidth == 64,
         "QQ widths");
   CHECK(x.laneCount == 4, "QQ256 lanes %u", x.laneCount);

   st = decode(B_EVEX_DD512_DISP8, (UInt32) sizeof(B_EVEX_DD512_DISP8), &x);
   CHECK(st == BEA_VSIB_OK, "EVEX DD512 status %d", st);
   CHECK(x.encoding == BEA_VSIB_ENCODING_EVEX, "EVEX encoding");
   CHECK(x.vectorBits == 512 && x.indexWidth == 32 && x.dataWidth == 32,
         "DD512 widths");
   CHECK(x.laneCount == 16, "DD512 lanes %u", x.laneCount);
   CHECK(x.instructionLength == 8, "DD512 length %u", x.instructionLength);
   CHECK(x.displacement == 8, "compressed disp8 -> %lld",
         (long long) x.displacement);
   CHECK(x.maskKind == BEA_VSIB_MASK_K && x.maskRegister == 1, "k1 mask");

   st = decode(B_EVEX_DQ128, (UInt32) sizeof(B_EVEX_DQ128), &x);
   CHECK(st == BEA_VSIB_OK, "EVEX DQ128 status %d", st);
   CHECK(x.indexWidth == 64 && x.dataWidth == 32 && x.laneCount == 2,
         "DQ128 mixed widths lanes %u", x.laneCount);

   st = decode(B_EVEX_QD128_SCATTER,
               (UInt32) sizeof(B_EVEX_QD128_SCATTER), &x);
   CHECK(st == BEA_VSIB_OK, "EVEX QD128 scatter status %d", st);
   CHECK(x.operation == BEA_VSIB_SCATTER, "scatter op");
   CHECK(x.indexWidth == 32 && x.dataWidth == 64 && x.laneCount == 2,
         "QD128 mixed widths lanes %u", x.laneCount);

   st = decode(B_EVEX_DD512_IDX31, (UInt32) sizeof(B_EVEX_DD512_IDX31), &x);
   CHECK(st == BEA_VSIB_OK, "idx31 status %d", st);
   CHECK(x.indexRegister == 31, "highest index register %u",
         x.indexRegister);

   st = decode(B_EVEX_BASELESS, (UInt32) sizeof(B_EVEX_BASELESS), &x);
   CHECK(st == BEA_VSIB_OK, "baseless status %d", st);
   CHECK(x.hasBase == 0 && x.baseRegister == BEA_VSIB_NO_REG,
         "base-less SIB must not impersonate rbp");
   CHECK(x.displacement == 0x12345678, "disp32 verbatim %lld",
         (long long) x.displacement);
   CHECK(x.instructionLength == 11, "baseless length %u",
         x.instructionLength);

   st = decode(B_VEX_FS_ADDR32, (UInt32) sizeof(B_VEX_FS_ADDR32), &x);
   CHECK(st == BEA_VSIB_OK, "fs/67 status %d", st);
   CHECK(x.addressSize == 32, "67h address size %u", x.addressSize);
   CHECK(x.segment == FSReg, "FS segment %u", x.segment);

   st = decode(B_EVEX_DISP32_POS, (UInt32) sizeof(B_EVEX_DISP32_POS), &x);
   CHECK(st == BEA_VSIB_OK, "disp32 status %d", st);
   CHECK(x.displacement == 8, "disp32 must not be rescaled (%lld)",
         (long long) x.displacement);

   /* error paths: explicit status and no residue left in the descriptor */
   (void) memset(&x, 0xAB, sizeof(x));
   st = decode(B_TRUNCATED, (UInt32) sizeof(B_TRUNCATED), &x);
   CHECK(st == BEA_VSIB_ERR_TRUNCATED, "truncated status %d", st);
   CHECK(x.encoding == 0 && x.instructionLength == 0, "truncated residue");

   st = decode(B_MOV, (UInt32) sizeof(B_MOV), &x);
   CHECK(st == BEA_VSIB_ERR_NOT_VSIB, "non-VSIB status %d", st);
   st = decode(B_REG_FORM, (UInt32) sizeof(B_REG_FORM), &x);
   CHECK(st == BEA_VSIB_ERR_NOT_VSIB, "register form status %d", st);
   st = decode(B_K0, (UInt32) sizeof(B_K0), &x);
   CHECK(st == BEA_VSIB_ERR_BAD_ENCODING, "k0 status %d", st);
   CHECK(BeaVsibDecode(NULL, &x) == BEA_VSIB_ERR_BAD_ARGUMENT, "null disasm");
   {
      DISASM d;
      (void) memset(&d, 0, sizeof(d));
      CHECK(BeaVsibDecode(&d, NULL) == BEA_VSIB_ERR_BAD_ARGUMENT,
            "null desc");
   }

   if (g_failures == 0) {
      printf("PASS encoding\n");
   }
}

/* ------------------------------------------------------------------ */
/* group 2: lane activation, masks, ordering, attributes               */
/* ------------------------------------------------------------------ */

static void test_lanes(void)
{
   BeaVsibDesc d;
   BeaVsibSnapshot s;
   BeaVsibRecord r[BEA_VSIB_MAX_LANES];
   UInt32 count;
   int st;
   int i;

   (void) memset(&s, 0, sizeof(s));

   /* EVEX k-mask on a 16-lane DD512: only lanes 0, 2 and the high bit 15
    * are active; everything else is masked off. */
   st = decode(B_EVEX_DD512_DISP8, (UInt32) sizeof(B_EVEX_DD512_DISP8), &d);
   CHECK(st == BEA_VSIB_OK, "lanes decode %d", st);
   s.gpr[0] = 0x1000;
   for (i = 0; i < 16; i++) {
      put_u32(s.vector[3] + i * 4, (UInt32) (i + 1));
   }
   s.k[1] = (1u << 0) | (1u << 2) | (1u << 15);
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 3, "EVEX mask count %u", count);
   if (count == 3) {
      CHECK(r[0].lane == 0 && r[1].lane == 2 && r[2].lane == 15,
            "EVEX lane order");
      CHECK(r[0].access == BEA_VSIB_ACCESS_READ, "gather reads");
      CHECK(r[0].accessBytes == 4, "dd access bytes %u",
            r[0].accessBytes);
   }

   /* Bits above the effective lane count are ignored: an 8-lane vector
    * with a full 16-bit mask still yields eight records. */
   st = decode(B_EVEX_DD256, (UInt32) sizeof(B_EVEX_DD256), &d);
   CHECK(st == BEA_VSIB_OK, "DD256 decode %d", st);
   s.k[1] = 0xFFFFu;
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 8, "high mask bits ignored (%u)",
         count);

   /* All-zero mask -> empty footprint. */
   s.k[1] = 0;
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 0, "zero mask empty (%u)", count);

   /* VEX sign mask on a 4-lane DD128: lanes 1 and 3 own the MSB of their
    * 32-bit mask element. */
   st = decode(B_VEX_DD128, (UInt32) sizeof(B_VEX_DD128), &d);
   CHECK(st == BEA_VSIB_OK, "VEX DD128 decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x2000;
   for (i = 0; i < 4; i++) {
      put_u32(s.vector[3] + i * 4, (UInt32) (100 + i));
      put_u32(s.vector[2] + i * 4, (i == 1 || i == 3) ? 0x80000000u : 0);
   }
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 2, "VEX sign mask count %u", count);
   if (count == 2) {
      CHECK(r[0].lane == 1 && r[1].lane == 3, "VEX sign lane order");
   }

   /* VEX mask element width follows the data width: a QD gather uses the
    * sign bit of each 64-bit mask element. */
   st = decode(B_VEX_QD256, (UInt32) sizeof(B_VEX_QD256), &d);
   CHECK(st == BEA_VSIB_OK, "VEX QD256 decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x3000;
   put_u64(s.vector[2] + 0, 0x8000000000000000ull); /* lane 0 active */
   put_u64(s.vector[2] + 8, 0x7fffffffffffffffull); /* lane 1 off */
   for (i = 0; i < 4; i++) {
      put_u32(s.vector[3] + i * 4, (UInt32) i);
   }
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 1 && r[0].lane == 0,
         "64-bit VEX sign element");

   /* Negative index is preserved and two lanes may share an address. */
   st = decode(B_EVEX_DD256, (UInt32) sizeof(B_EVEX_DD256), &d);
   CHECK(st == BEA_VSIB_OK, "dup decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x4000;
   put_u32(s.vector[3] + 0, 0xFFFFFFFCu); /* -4 */
   put_u32(s.vector[3] + 4, 0xFFFFFFFCu); /* same -4, same address */
   s.k[1] = 0x3;
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 2, "duplicate lanes kept (%u)",
         count);
   if (count == 2) {
      CHECK(r[0].indexValue == -4 && r[1].indexValue == -4,
            "signed index %lld", (long long) r[0].indexValue);
      CHECK(r[0].address == r[1].address && r[0].lane == 0
            && r[1].lane == 1, "duplicate address records");
   }

   /* Scatter writes, and a short buffer is refused cleanly. */
   st = decode(B_EVEX_QD128_SCATTER,
               (UInt32) sizeof(B_EVEX_QD128_SCATTER), &d);
   CHECK(st == BEA_VSIB_OK, "scatter decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x5000;
   s.k[1] = 0x2; /* only lane 1 */
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 1, "scatter count %u", count);
   if (count == 1) {
      CHECK(r[0].access == BEA_VSIB_ACCESS_WRITE && r[0].lane == 1,
            "scatter write attribute");
      CHECK(r[0].accessBytes == 8, "qd access bytes %u",
            r[0].accessBytes);
   }
   st = footprint(&d, &s, r, 0, &count);
   CHECK(st == BEA_VSIB_ERR_NO_SPACE, "capacity status %d", st);

   /* The highest vector register supplies lane bytes. */
   st = decode(B_EVEX_DD512_IDX31, (UInt32) sizeof(B_EVEX_DD512_IDX31), &d);
   CHECK(st == BEA_VSIB_OK, "idx31 decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x6000;
   put_u32(s.vector[31] + 0, 7u);
   s.k[1] = 1;
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 1 && r[0].address == 0x601c,
         "zmm31 lane address 0x%llx",
         (unsigned long long) (count ? r[0].address : 0));

   if (g_failures == 0) {
      printf("PASS lanes\n");
   }
}

/* ------------------------------------------------------------------ */
/* group 3: address arithmetic against independent expected tables     */
/* ------------------------------------------------------------------ */

typedef struct {
   UInt8  lane;
   UInt8  status;
   UInt64 address;
   Int64  indexValue;
} ExpectedAddr;

static void test_addresses(void)
{
   BeaVsibDesc d;
   BeaVsibSnapshot s;
   BeaVsibRecord r[BEA_VSIB_MAX_LANES];
   UInt32 count;
   int st;
   int i;

   /* Basic 32-bit indices, including a negative one and a large one.
    * EVEX128 DD: base 0x1000, scale 4, four active lanes. */
   static const ExpectedAddr expect_basic[4] = {
      {0, BEA_VSIB_ADDRESS_VALID, 0x0000000000001028ull, 10},
      {1, BEA_VSIB_ADDRESS_VALID, 0x0000000000001050ull, 20},
      {2, BEA_VSIB_ADDRESS_VALID, 0x0000000000000ffcull, -1},
      {3, BEA_VSIB_ADDRESS_VALID, 0x0000000100001000ull, 0x40000000},
   };
   st = decode(B_EVEX_DD128_REF, 7, &d);
   CHECK(st == BEA_VSIB_OK, "basic decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x1000;
   put_u32(s.vector[3] + 0, 10);
   put_u32(s.vector[3] + 4, 20);
   put_u32(s.vector[3] + 8, 0xffffffffu);
   put_u32(s.vector[3] + 12, 0x40000000u);
   s.k[1] = 0xF;
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 4, "basic count %u", count);
   for (i = 0; i < 4 && i < (int) count; i++) {
      CHECK(r[i].lane == expect_basic[i].lane
            && r[i].status == expect_basic[i].status
            && r[i].address == expect_basic[i].address
            && r[i].indexValue == expect_basic[i].indexValue,
            "basic lane %d: got 0x%llx st%u idx%lld want 0x%llx idx%lld",
            i, (unsigned long long) r[i].address, r[i].status,
            (long long) r[i].indexValue,
            (unsigned long long) expect_basic[i].address,
            (long long) expect_basic[i].indexValue);
   }

   /* 64-bit indices on an 8-byte QQ gather. */
   static const ExpectedAddr expect_qq[4] = {
      {0, BEA_VSIB_ADDRESS_VALID, 0x0000000000200004ull, 1},
      {1, BEA_VSIB_ADDRESS_VALID, 0x0000000000200008ull, 2},
      {2, BEA_VSIB_ADDRESS_VALID, 0x00000000001ffffcull, -1},
      {3, BEA_VSIB_ADDRESS_VALID, 0x0000000000200000ull,
        (Int64) 0x8000000000000000ull},
   };
   st = decode(B_EVEX_QQ512, 7, &d);
   CHECK(st == BEA_VSIB_OK, "qq decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x200000;
   put_u64(s.vector[3] + 0, 1);
   put_u64(s.vector[3] + 8, 2);
   put_u64(s.vector[3] + 16, 0xffffffffffffffffull);
   put_u64(s.vector[3] + 24, 0x8000000000000000ull);
   s.k[1] = 0xF;
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 4, "qq count %u", count);
   for (i = 0; i < 4 && i < (int) count; i++) {
      CHECK(r[i].lane == expect_qq[i].lane
            && r[i].address == expect_qq[i].address
            && r[i].indexValue == expect_qq[i].indexValue,
            "qq lane %d: got 0x%llx want 0x%llx", i,
            (unsigned long long) r[i].address,
            (unsigned long long) expect_qq[i].address);
   }

   /* 32-bit modulus first, then the FS base; a 32-bit access never
    * re-wraps its own tail. */
   static const ExpectedAddr expect_fs[2] = {
      {0, BEA_VSIB_ADDRESS_VALID, 0x00007f0000001004ull, 1},
      {1, BEA_VSIB_ADDRESS_VALID, 0x00007f0000000ffcull, -1},
   };
   st = decode(B_VEX_FS_ADDR32, (UInt32) sizeof(B_VEX_FS_ADDR32), &d);
   CHECK(st == BEA_VSIB_OK, "fs decode %d", st);
   (void) memset(&s, 0, sizeof(s));
   s.gpr[0] = 0x100001000ull;          /* low 32 bits leave 0x1000 */
   s.fsBase = 0x00007f0000000000ull;
   put_u32(s.vector[3] + 0, 1);
   put_u32(s.vector[3] + 4, 0xffffffffu);
   put_u32(s.vector[2] + 0, 0x80000000u);
   put_u32(s.vector[2] + 4, 0x80000000u);
   st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
   CHECK(st == BEA_VSIB_OK && count == 2, "fs count %u", count);
   for (i = 0; i < 2 && i < (int) count; i++) {
      CHECK(r[i].lane == expect_fs[i].lane
            && r[i].address == expect_fs[i].address,
            "fs lane %d: got 0x%llx want 0x%llx", i,
            (unsigned long long) r[i].address,
            (unsigned long long) expect_fs[i].address);
   }

   /* Compressed disp8 and full disp32 produce identical footprints. */
   {
      BeaVsibRecord r2[BEA_VSIB_MAX_LANES];
      UInt32 count2;
      BeaVsibDesc d2;
      st = decode(B_EVEX_DISP8_NEG, (UInt32) sizeof(B_EVEX_DISP8_NEG), &d);
      CHECK(st == BEA_VSIB_OK, "disp8 neg decode %d", st);
      st = decode(B_EVEX_DISP32_NEG, (UInt32) sizeof(B_EVEX_DISP32_NEG),
                  &d2);
      CHECK(st == BEA_VSIB_OK, "disp32 neg decode %d", st);
      (void) memset(&s, 0, sizeof(s));
      s.gpr[0] = 0x8000;
      for (i = 0; i < 16; i++) {
         put_u32(s.vector[3] + i * 4, (UInt32) i);
      }
      s.k[1] = 0xFFFFu;
      st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
      CHECK(st == BEA_VSIB_OK, "disp8 footprint %d", st);
      st = footprint(&d2, &s, r2, BEA_VSIB_MAX_LANES, &count2);
      CHECK(st == BEA_VSIB_OK, "disp32 footprint %d", st);
      CHECK(count == count2, "disp equivalence count %u/%u", count, count2);
      for (i = 0; i < (int) count; i++) {
         CHECK(r[i].address == r2[i].address && r[i].lane == r2[i].lane,
               "negative disp equivalence lane %d", i);
      }

      st = decode(B_EVEX_DISP8_POS, (UInt32) sizeof(B_EVEX_DISP8_POS), &d);
      CHECK(st == BEA_VSIB_OK, "disp8 pos decode %d", st);
      st = decode(B_EVEX_DISP32_POS, (UInt32) sizeof(B_EVEX_DISP32_POS),
                  &d2);
      CHECK(st == BEA_VSIB_OK, "disp32 pos decode %d", st);
      st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
      CHECK(st == BEA_VSIB_OK, "disp8 pos footprint %d", st);
      st = footprint(&d2, &s, r2, BEA_VSIB_MAX_LANES, &count2);
      CHECK(st == BEA_VSIB_OK, "disp32 pos footprint %d", st);
      for (i = 0; i < (int) count; i++) {
         CHECK(r[i].address == r2[i].address,
               "positive disp equivalence lane %d", i);
      }
   }

   /* Canonical-hole lane and uint64 tail overflow are kept invalid while
    * sibling lanes still report. */
   {
      static const ExpectedAddr expect_bad[2] = {
         {0, BEA_VSIB_ADDRESS_VALID, 0x0000000000000004ull, 1},
         {1, BEA_VSIB_ADDRESS_INVALID, 0, 0x200000000000ll},
      };
      st = decode(B_EVEX_DQ128, (UInt32) sizeof(B_EVEX_DQ128), &d);
      CHECK(st == BEA_VSIB_OK, "hole decode %d", st);
      (void) memset(&s, 0, sizeof(s));
      s.gpr[0] = 0;
      put_u64(s.vector[3] + 0, 1);
      put_u64(s.vector[3] + 8, 0x0000200000000000ull); /* *4 = hole start */
      s.k[1] = 0x3;
      st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
      CHECK(st == BEA_VSIB_OK && count == 2, "hole count %u", count);
      for (i = 0; i < 2 && i < (int) count; i++) {
         CHECK(r[i].lane == expect_bad[i].lane
               && r[i].status == expect_bad[i].status
               && r[i].address == expect_bad[i].address
               && r[i].indexValue == expect_bad[i].indexValue,
               "hole lane %d st%u addr0x%llx", i, r[i].status,
               (unsigned long long) r[i].address);
      }
   }
   {
      /* 4-byte access starting at ...FFFFFFFE wraps its tail. */
      BeaVsibDesc dd;
      static const unsigned char tail_bytes[] =
         {0x62,0xf2,0x7d,0x09,0x90,0x0c,0x98};
      st = decode(tail_bytes, (UInt32) sizeof(tail_bytes), &dd);
      CHECK(st == BEA_VSIB_OK, "tail decode %d", st);
      (void) memset(&s, 0, sizeof(s));
      s.gpr[0] = 0xfffffffffffffffeull;
      put_u32(s.vector[3] + 0, 0); /* lane 0 at the very top */
      put_u32(s.vector[3] + 4, 1); /* lane 1 one element below */
      s.k[1] = 0x3;
      st = footprint(&dd, &s, r, BEA_VSIB_MAX_LANES, &count);
      CHECK(st == BEA_VSIB_OK && count == 2, "tail count %u", count);
      if (count == 2) {
         CHECK(r[0].status == BEA_VSIB_ADDRESS_INVALID
               && r[0].lane == 0, "tail overflow invalid");
         CHECK(r[1].status == BEA_VSIB_ADDRESS_VALID
               && r[1].address == 0x0000000000000002ull,
               "sibling lane survives 0x%llx",
               (unsigned long long) r[1].address);
      }
   }

   /* High-canonical access spanning the top of the address space stays
    * valid as long as the whole 8-byte run is representable. */
   {
      st = decode(B_EVEX_QQ128, (UInt32) sizeof(B_EVEX_QQ128), &d);
      CHECK(st == BEA_VSIB_OK, "high canonical decode %d", st);
      (void) memset(&s, 0, sizeof(s));
      s.gpr[0] = 0xfffffffffffffff8ull;
      s.k[1] = 0x1;
      st = footprint(&d, &s, r, BEA_VSIB_MAX_LANES, &count);
      CHECK(st == BEA_VSIB_OK && count == 1, "high count %u", count);
      if (count == 1) {
         CHECK(r[0].status == BEA_VSIB_ADDRESS_VALID
               && r[0].address == 0xfffffffffffffff8ull,
               "top 8 bytes valid");
      }
   }

   if (g_failures == 0) {
      printf("PASS addresses\n");
   }
}

int main(void)
{
   test_encoding();
   test_lanes();
   test_addresses();
   if (g_failures != 0) {
      printf("VSIB_FOOTPRINT_FAILURES=%d\n", g_failures);
      return 1;
   }
   printf("VSIB_FOOTPRINT_OK\n");
   return 0;
}
