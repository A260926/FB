/*  Contract test for the AADWARF64 2026Q3 AArch64 return
    address signing metadata in .debug_frame and .eh_frame.

    It builds small ELF objects in C (no machine code, no
    external packages), writes them to a temporary file and
    exercises the public libdwarf frame interfaces:

      - dwarf_get_fde_ra_state()
      - dwarf_get_fde_info_for_cfa_reg3_c()
      - dwarf_get_fde_info_for_reg3_c()
      - dwarf_get_fde_info_for_all_regs3_b()
      - dwarf_expand_frame_instructions()
      - dwarf_get_frame_instruction_a()

    The RA state is checked together with the ordinary CFA and
    register rows so that the shared row model, the caching and
    the instruction decoding are all covered.

    This file defines no main().  It registers TASK_CHECK cases
    in the Rows, State and Decode groups.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <process.h>
#define TEST_GETPID() _getpid()
#else
#include <unistd.h>
#define TEST_GETPID() getpid()
#endif

#include "libdwarf.h"
#include "dwarf.h"
#include "task_support/check.h"

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/*  ----------------------------------------------------------------- */
/*  Tiny byte buffer and LEB encoders.                                 */
/*  ----------------------------------------------------------------- */

typedef struct {
    unsigned char b[16384];
    unsigned      n;
} Buf;

static void buf_u8(Buf *b, unsigned v)
{
    b->b[b->n++] = (unsigned char)(v & 0xff);
}

static void buf_u32(Buf *b, unsigned v)
{
    unsigned i = 0;
    for (i = 0; i < 4; ++i) {
        b->b[b->n++] = (unsigned char)((v >> (8*i)) & 0xff);
    }
}

static void buf_u64(Buf *b, Dwarf_Unsigned v)
{
    unsigned i = 0;
    for (i = 0; i < 8; ++i) {
        b->b[b->n++] = (unsigned char)((v >> (8*i)) & 0xff);
    }
}

static void buf_uleb(Buf *b, Dwarf_Unsigned v)
{
    do {
        unsigned char c = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v) {
            c |= 0x80;
        }
        buf_u8(b, c);
    } while (v);
}

static void buf_sleb(Buf *b, Dwarf_Signed v)
{
    int more = 1;
    while (more) {
        unsigned char c = (unsigned char)(v & 0x7f);
        v >>= 7;
        if ((v == 0 && !(c & 0x40)) ||
            (v == -1 && (c & 0x40))) {
            more = 0;
        } else {
            c |= 0x80;
        }
        buf_u8(b, c);
    }
}

static void buf_raw(Buf *b, const unsigned char *d, unsigned n)
{
    memcpy(b->b+b->n,d,n);
    b->n += n;
}

/*  Pad with zero bytes to a 4-byte boundary. */
static void buf_pad4(Buf *b)
{
    while (b->n % 4) {
        buf_u8(b,0);
    }
}

/*  ----------------------------------------------------------------- */
/*  Frame section and ELF construction.                                */
/*  ----------------------------------------------------------------- */

typedef struct {
    Buf  sec;      /* The .debug_frame or .eh_frame bytes. */
    int  eh;       /* Non-zero for .eh_frame. */
} Fx;

static void fx_init(Fx *f, int eh)
{
    memset(f,0,sizeof(*f));
    f->eh = eh;
}

/*  Emit a CIE and return its section offset.  The augmentation
    string is the caller's, e.g. "" or "B" or "zRBP".  For an
    .eh_frame z-style CIE the augmentation data that the R, L
    and P letters require is generated in the correct order;
    the B letter consumes no augmentation data at all. */
static unsigned fx_cie(Fx *f, const char *aug,
    unsigned code_align, Dwarf_Signed data_align,
    unsigned ra_reg,
    const unsigned char *instr, unsigned ilen)
{
    Buf *b = &f->sec;
    unsigned start = b->n;
    unsigned length_pos = b->n;
    unsigned i = 0;

    buf_u32(b,0);                 /* length, patched later */
    buf_u32(b, f->eh ? 0 : 0xffffffffu); /* CIE id */
    buf_u8(b,1);                  /* version */
    buf_raw(b,(const unsigned char *)aug,
        (unsigned)strlen(aug)+1);
    buf_uleb(b,code_align);
    buf_sleb(b,data_align);
    buf_u8(b,ra_reg);
    if (f->eh && aug[0] == 'z') {
        /*  Build the augmentation data bytes for the letters
            after the leading 'z'. */
        Buf ad;
        memset(&ad,0,sizeof(ad));
        for (i = 1; aug[i]; ++i) {
            switch (aug[i]) {
            case 'L':
                buf_u8(&ad,0x00); /* lsda is absptr */
                break;
            case 'R':
                buf_u8(&ad,0x00); /* fde begin is absptr */
                break;
            case 'P':
                buf_u8(&ad,0x00); /* personality absptr */
                buf_u64(&ad,0x4000);
                break;
            case 'B':
                /* Consumes no augmentation data. */
                break;
            default:
                return (unsigned)-1; /* test bug */
            }
        }
        buf_uleb(b,ad.n);
        buf_raw(b,ad.b,ad.n);
    }
    buf_raw(b,instr,ilen);
    /*  Legal CIE nop padding.  It keeps every fixture clear of an
        unrelated upstream minimum-record-count heuristic in
        dwarf_frame2.c and changes no frame rule. */
    while (b->n - start < 48) {
        buf_u8(b,0);
    }
    buf_pad4(b);
    {
        unsigned len = b->n - length_pos - 4;
        b->b[length_pos+0] = (unsigned char)(len & 0xff);
        b->b[length_pos+1] = (unsigned char)((len>>8) & 0xff);
        b->b[length_pos+2] = (unsigned char)((len>>16) & 0xff);
        b->b[length_pos+3] = (unsigned char)((len>>24) & 0xff);
    }
    return start;
}

static void fx_fde(Fx *f, unsigned cie_start, int eh_z,
    Dwarf_Addr initloc, Dwarf_Addr range,
    const unsigned char *instr, unsigned ilen)
{
    Buf *b = &f->sec;
    unsigned length_pos = b->n;

    buf_u32(b,0);
    if (f->eh) {
        /*  CIE pointer is the distance from this field back
            to the CIE length field. */
        unsigned cie_id_field = b->n;
        buf_u32(b,(unsigned)(cie_id_field - cie_start));
    } else {
        buf_u32(b,cie_start);
    }
    /*  With the R encoding absptr (0) both initial location and
        range are plain pointer-size values in .eh_frame. */
    buf_u64(b,initloc);
    buf_u64(b,range);
    if (f->eh && eh_z) {
        /*  z-style FDE augmentation data length.  The test
            uses no per-FDE letters, so it is zero. */
        buf_uleb(b,0);
    }
    buf_raw(b,instr,ilen);
    buf_pad4(b);
    {
        unsigned len = b->n - length_pos - 4;
        b->b[length_pos+0] = (unsigned char)(len & 0xff);
        b->b[length_pos+1] = (unsigned char)((len>>8) & 0xff);
        b->b[length_pos+2] = (unsigned char)((len>>16) & 0xff);
        b->b[length_pos+3] = (unsigned char)((len>>24) & 0xff);
    }
}

/*  Write an ELF64 little-endian relocatable object with one
    section named .debug_frame or .eh_frame. */
static int fx_write(const char *path, unsigned machine, Fx *f)
{
    static unsigned char b[32768];
    const char names[] = "\0.shstrtab\0.debug_frame\0.eh_frame\0";
    unsigned foff = 128;
    unsigned flen = f->sec.n;
    unsigned sh = 0;
    unsigned nameoff = f->eh ? 24 : 11;
    FILE *out = 0;
    size_t want = 0;
    int bad = 0;

    memset(b,0,sizeof(b));
    memcpy(b,"\177ELF\2\1\1",7);
    b[16] = 1; b[17] = 0;                 /* ET_REL */
    b[18] = (unsigned char)(machine & 0xff);
    b[19] = (unsigned char)((machine>>8) & 0xff);
    b[20] = 1; b[21] = 0; b[22] = 0; b[23] = 0;
    b[52] = 64; b[53] = 0;                /* ehsize */
    b[58] = 64; b[59] = 0;                /* shentsize */
    b[60] = 3;  b[61] = 0;                /* shnum */
    b[62] = 1;  b[63] = 0;                /* shstrndx */
    memcpy(b+64,names,sizeof(names));
    memcpy(b+foff,f->sec.b,flen);
    sh = (foff + flen + 7u) & ~7u;
    /* e_shoff */
    b[40] = (unsigned char)(sh & 0xff);
    b[41] = (unsigned char)((sh>>8) & 0xff);
    b[42] = (unsigned char)((sh>>16) & 0xff);
    b[43] = (unsigned char)((sh>>24) & 0xff);
    /* null section header already zero */
    /* .shstrtab at sh+64 */
    b[sh+64+0] = 1;                       /* name offset */
    b[sh+64+4] = 3;                       /* SHT_STRTAB */
    b[sh+64+24] = 64;                     /* offset */
    b[sh+64+32] = (unsigned char)sizeof(names);
    b[sh+64+48] = 1;                      /* align */
    /* frame section at sh+128 */
    b[sh+128+0] = (unsigned char)nameoff;
    b[sh+128+4] = 1;                      /* SHT_PROGBITS */
    b[sh+128+24] = (unsigned char)foff;
    b[sh+128+32] = (unsigned char)(flen & 0xff);
    b[sh+128+33] = (unsigned char)((flen>>8) & 0xff);
    b[sh+128+48] = 8;                     /* align */
    want = sh + 192;
    out = fopen(path,"wb");
    if (!out) {
        return 1;
    }
    bad = fwrite(b,1,want,out) != want;
    if (fclose(out)) {
        bad = 1;
    }
    return bad;
}

/*  ----------------------------------------------------------------- */
/*  Opening a fixture and shared query helpers.                        */
/*  ----------------------------------------------------------------- */

typedef struct {
    Dwarf_Debug    dbg;
    Dwarf_Cie     *cies;
    Dwarf_Fde     *fdes;
    Dwarf_Signed   nc;
    Dwarf_Signed   nf;
    Dwarf_Error    err;
} Opened;

static int open_fixture(Opened *o, const char *path, int eh)
{
    int res = 0;

    memset(o,0,sizeof(*o));
    res = dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,0,0,
        &o->dbg,&o->err);
    if (res != DW_DLV_OK) {
        return res;
    }
    /*  Keep the rule table small so a caller-provided all-regs
        table of the same size is safe. */
    dwarf_set_frame_rule_table_size(o->dbg,64);
    res = eh ? dwarf_get_fde_list_eh(o->dbg,&o->cies,&o->nc,
            &o->fdes,&o->nf,&o->err)
        : dwarf_get_fde_list(o->dbg,&o->cies,&o->nc,
            &o->fdes,&o->nf,&o->err);
    return res;
}

static void close_fixture(Opened *o)
{
    if (o->fdes || o->cies) {
        dwarf_dealloc_fde_cie_list(o->dbg,o->cies,o->nc,
            o->fdes,o->nf);
        o->fdes = 0;
        o->cies = 0;
    }
    dwarf_finish(o->dbg);
    o->dbg = 0;
}

static void clear_err(Opened *o)
{
    if (o->err) {
        dwarf_dealloc_error(o->dbg,o->err);
        o->err = 0;
    }
}

/*  Query the RA state and return the values through out
    parameters.  Returns the libdwarf result code. */
static int ra_state(Opened *o, Dwarf_Fde fde, Dwarf_Addr pc,
    Dwarf_Ra_State *st, Dwarf_Addr *row_pc,
    Dwarf_Bool *more, Dwarf_Addr *next)
{
    return dwarf_get_fde_ra_state(fde,pc,st,row_pc,more,next,
        &o->err);
}

/*  ----------------------------------------------------------------- */
/*  Instruction streams.                                              */
/*  ----------------------------------------------------------------- */

/*  CFI: offset rule LR(30) = factored 2, then
    advance_loc 1, set_ra_state(state 2, offset 3),
    advance_loc 4, negate_ra_state_with_pc (2 -> 0),
    advance_loc 1, negate_ra_state_with_pc (0 -> 2),
    advance_loc 1.

    Rows, relative to the FDE low pc, are at +0, +4, +20,
    +24 and +28.  State 2 signings are low+16 and low+24. */
static unsigned seq_main(unsigned char *out)
{
    unsigned n = 0;

    out[n++] = 0x9e;         /* DW_CFA_offset reg 30 */
    out[n++] = 0x02;         /* factored 2, data_align -8 */
    out[n++] = 0x41;         /* advance_loc 1 -> +4 */
    out[n++] = 0x2b;         /* DW_CFA_AARCH64_set_ra_state */
    out[n++] = 0x02;         /*   state 2 */
    out[n++] = 0x03;         /*   offset 3 */
    out[n++] = 0x44;         /* advance_loc 4 -> +20 */
    out[n++] = 0x2c;         /* negate_ra_state_with_pc 2 -> 0 */
    out[n++] = 0x41;         /* advance_loc 1 -> +24 */
    out[n++] = 0x2c;         /* negate_ra_state_with_pc 0 -> 2 */
    out[n++] = 0x41;         /* advance_loc 1 -> +28 */
    return n;
}

/*  Same idea with a negative (multi-byte) set offset:
    advance_loc 1 -> +4, set_ra_state(state 2, offset -129)
    gives a signing pc of low + 4 - 129*4 = low - 512. */
static unsigned seq_negative(unsigned char *out)
{
    unsigned n = 0;
    Buf t;

    memset(&t,0,sizeof(t));
    buf_u8(&t,0x41);
    buf_u8(&t,0x2b);
    buf_uleb(&t,2);
    buf_sleb(&t,-129);
    buf_u8(&t,0x41);
    memcpy(out,t.b,t.n);
    n = t.n;
    return n;
}

/*  CIE initial state 1 via a negate_ra_state in the CIE.
    The FDE then returns to state 0 after one row. */
static unsigned cie_state_one(unsigned char *out)
{
    unsigned n = 0;

    out[n++] = 0x0c;  /* def_cfa */
    out[n++] = 31;    /* SP */
    out[n++] = 16;    /* offset 16 */
    out[n++] = 0x2d;  /* negate_ra_state 0 -> 1 */
    return n;
}

static unsigned fde_state_one(unsigned char *out)
{
    unsigned n = 0;

    out[n++] = 0x41;  /* advance_loc 1 -> +4 */
    out[n++] = 0x2d;  /* 1 -> 0 */
    out[n++] = 0x41;  /* advance_loc 1 -> +8 */
    return n;
}

/*  Nested remember/restore.  Two save tokens are pushed so
    the two restores pair with them properly:

      0x2d                    state 0 -> 1            row +0
      0x0a  remember A        saves state 1, sign 0
      0x41  advance +4                                 row +4
      0x2d                    state 1 -> 0
      0x2c                    state 0 -> 2, sign +4
      0x0a  remember B        saves state 2, sign +4
      0x42  advance +8                                 row +12
      0x0b  restore B         back to state 2, sign +4
      0x41  advance +4                                 row +16
      0x0b  restore A         back to state 1, sign 0
      0x41  advance +4                                 row +20
      0x2d                    state 1 -> 0
    Restoring never rewinds the current location.
    */
static unsigned seq_remember(unsigned char *out)
{
    unsigned n = 0;

    out[n++] = 0x2d;  /* 0 -> 1 */
    out[n++] = 0x0a;  /* remember A */
    out[n++] = 0x41;  /* +4 */
    out[n++] = 0x2d;  /* 1 -> 0 */
    out[n++] = 0x2c;  /* 0 -> 2, signing +4 */
    out[n++] = 0x0a;  /* remember B */
    out[n++] = 0x42;  /* +8 */
    out[n++] = 0x0b;  /* restore B -> state 2, sign +4 */
    out[n++] = 0x41;  /* +4 */
    out[n++] = 0x0b;  /* restore A -> state 1, sign 0 */
    out[n++] = 0x41;  /* +4 */
    out[n++] = 0x2d;  /* 1 -> 0 */
    return n;
}

/*  Shared CIE, per-FDE instruction: advance 1 then enter state 2. */
static unsigned fde_shared(unsigned char *out)
{
    unsigned n = 0;

    out[n++] = 0x41;
    out[n++] = 0x2c;
    return n;
}

/*  Non-AArch64 object: 0x2d must keep the GNU window_save
    meaning (a no-op) and the CFA rule must still work. */
static unsigned seq_non_aarch64(unsigned char *out)
{
    unsigned n = 0;

    out[n++] = 0x9e;  /* LR offset */
    out[n++] = 0x02;
    out[n++] = 0x41;
    out[n++] = 0x2d;  /* GNU_window_save, no-op */
    out[n++] = 0x41;
    return n;
}

/*  ----------------------------------------------------------------- */
/*  Fixture paths.                                                    */
/*  ----------------------------------------------------------------- */

static void tmp_name(char *buf, size_t buflen, const char *tag)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) {
        dir = "/tmp";
    }
    snprintf(buf,buflen,"%s/libdwarf_ra_%s_%d.elf",dir,tag,
        (int)TEST_GETPID());
}

/*  Build one AArch64 object holding a single FDE and write it.
    mode_debug selects .debug_frame, otherwise .eh_frame.
    aug is the CIE augmentation string. */
static int build_one(const char *path, int eh, unsigned machine,
    const char *aug, unsigned code_align,
    const unsigned char *cie_instr, unsigned cie_len,
    Dwarf_Addr initloc, Dwarf_Addr range,
    const unsigned char *fde_instr, unsigned fde_len)
{
    Fx f;
    unsigned cie = 0;
    int bad = 0;

    fx_init(&f,eh);
    cie = fx_cie(&f,aug,code_align,-8,30,cie_instr,cie_len);
    if (cie == (unsigned)-1) {
        return 1;
    }
    fx_fde(&f,cie,(aug[0] == 'z'),initloc,range,fde_instr,
        fde_len);
    bad = fx_write(path,machine,&f);
    return bad;
}

/*  ----------------------------------------------------------------- */
/*  Group Rows: ordinary CFA/register rows checked together with RA.  */
/*  ----------------------------------------------------------------- */

static int case_rows_debug_frame(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_main(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned flen = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed ci = 0;
    Dwarf_Small vt = 0;
    Dwarf_Unsigned rel = 0, reg = 0;
    Dwarf_Signed off = 0;
    Dwarf_Block blk;
    int res = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"rows_debug");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(o.nf == 1 && o.nc == 1);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&flen,&fb,
        &flen,&cf,&ci,&df,&o.err) == DW_DLV_OK);
    TASK_CHECK(low == 0x1000);

    /* Row containing low+2: state 0, CFA SP+16, LR offset -16. */
    res = ra_state(&o,o.fdes[0],low+2,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low);
    TASK_CHECK(st.state == 0 && st.signing_pc == 0);
    TASK_CHECK(more == TRUE);
    TASK_CHECK(next == low+4);

    memset(&blk,0,sizeof(blk));
    res = dwarf_get_fde_info_for_cfa_reg3_c(o.fdes[0],low+2,
        &vt,&rel,&reg,&off,&blk,&row,&more,&next,&o.err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low);
    TASK_CHECK(vt == DW_EXPR_OFFSET);
    TASK_CHECK(rel == 1);
    TASK_CHECK(reg == 31);
    TASK_CHECK(off == 16);
    TASK_CHECK(more == TRUE);
    TASK_CHECK(next == low+4);

    memset(&blk,0,sizeof(blk));
    res = dwarf_get_fde_info_for_reg3_c(o.fdes[0],30,low+2,
        &vt,&rel,&reg,&off,&blk,&row,&more,&next,&o.err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(vt == DW_EXPR_OFFSET);
    TASK_CHECK(rel == 1);
    TASK_CHECK(reg == DW_FRAME_CFA_COL3);
    TASK_CHECK(off == -16);

    /* Row containing low+6: state 2, signing at low+16, and the
       ordinary CFA/register rows must be the same. */
    res = ra_state(&o,o.fdes[0],low+6,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 2);
    TASK_CHECK(st.signing_pc == low+16);
    TASK_CHECK(more == TRUE);
    TASK_CHECK(next == low+20);

    res = dwarf_get_fde_info_for_cfa_reg3_c(o.fdes[0],low+6,
        &vt,&rel,&reg,&off,&blk,&row,&more,&next,&o.err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(reg == 31 && off == 16);
    TASK_CHECK(next == low+20);

    /* State 2 spans up to low+20; the signing pc must not be
       replaced by the queried pc. */
    res = ra_state(&o,o.fdes[0],low+18,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 2);
    TASK_CHECK(st.signing_pc == low+16);

    /* Back to unsigned after the second 0x2c. */
    res = ra_state(&o,o.fdes[0],low+22,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+20);
    TASK_CHECK(st.state == 0 && st.signing_pc == 0);

    /* State 2 re-entered at low+24; advancing to low+28 keeps
       that signing pc. */
    res = ra_state(&o,o.fdes[0],low+26,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+24);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+24);

    res = ra_state(&o,o.fdes[0],low+30,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+28);
    TASK_CHECK(st.state == 2);
    TASK_CHECK(st.signing_pc == low+24);

    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_rows_eh_frame(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_main(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned flen = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed ci = 0;
    Dwarf_Small vt = 0;
    Dwarf_Unsigned rel = 0, reg = 0;
    Dwarf_Signed off = 0;
    Dwarf_Block blk;
    int res = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"rows_eh");
    /*  zLRBP: L, R and P each consume augmentation data; the
        B marker between R and P must consume nothing so P does
        not shift. */
    TASK_CHECK(build_one(path,1,183,"zLRBP",4,cie,clen,
        0x2000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,1) == DW_DLV_OK);
    TASK_CHECK(o.nf == 1 && o.nc == 1);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&flen,&fb,
        &blen,&cf,&ci,&df,&o.err) == DW_DLV_OK);
    /*  The R encoding was absptr, so the FDE low pc is exactly
        the value written.  A mis-parse of the augmentation data
        would show up here. */
    TASK_CHECK(low == 0x2000);

    res = ra_state(&o,o.fdes[0],low+6,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+16);
    TASK_CHECK(st.b_key == TRUE);
    TASK_CHECK(more == TRUE && next == low+20);

    memset(&blk,0,sizeof(blk));
    res = dwarf_get_fde_info_for_cfa_reg3_c(o.fdes[0],low+6,
        &vt,&rel,&reg,&off,&blk,&row,&more,&next,&o.err);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(reg == 31 && off == 16);

    res = ra_state(&o,o.fdes[0],low+30,&st,&row,&more,&next);
    TASK_CHECK(res == DW_DLV_OK);
    TASK_CHECK(row == low+28);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+24);

    close_fixture(&o);
    remove(path);
    return 0;
}

/*  ----------------------------------------------------------------- */
/*  Group State: RA state transitions, caching and error handling.    */
/*  ----------------------------------------------------------------- */

static int case_state_initial_zero(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_main(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0;
    Dwarf_Bool more = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"state_zero");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(ra_state(&o,o.fdes[0],0x1000,&st,&row,
        &more,&next) == DW_DLV_OK);
    TASK_CHECK(st.state == 0);
    TASK_CHECK(st.signing_pc == 0);
    /*  No B marker means the A key. */
    TASK_CHECK(st.b_key == FALSE);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_cie_initial_one(void)
{
    char path[512];
    Opened o;
    unsigned char ci[16];
    unsigned clen = cie_state_one(ci);
    unsigned char fi[16];
    unsigned flen = fde_state_one(fi);
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned ignored = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed cidx = 0;

    tmp_name(path,sizeof(path),"state_cie1");
    TASK_CHECK(build_one(path,0,183,"",4,ci,clen,
        0x1000,0x40,fi,flen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,&fb,
        &blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);
    /*  The CIE leaves state 1, so the first FDE row is state 1. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(st.state == 1 && st.signing_pc == 0);
    /*  The CIE also set the CFA, so the ordinary row works too. */
    {
        Dwarf_Small vt = 0;
        Dwarf_Unsigned rel = 0, reg = 0;
        Dwarf_Signed off = 0;
        Dwarf_Block blk;
        memset(&blk,0,sizeof(blk));
        TASK_CHECK(dwarf_get_fde_info_for_cfa_reg3_c(o.fdes[0],
            low,&vt,&rel,&reg,&off,&blk,&row,&more,&next,
            &o.err) == DW_DLV_OK);
        TASK_CHECK(reg == 31 && off == 16);
    }
    /*  The FDE toggles back to state 0 at low+4. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+6,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 0 && st.signing_pc == 0);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_negative_offset(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_negative(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned ignored = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed cidx = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"state_neg");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x4000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,&fb,
        &blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);
    /*  advance_loc 1 -> low+4, then offset -129 with code
        alignment 4: signing pc is low+4-516. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+6,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 2);
    TASK_CHECK(st.signing_pc == low + 4 - 516);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_remember_restore(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_remember(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned ignored = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed cidx = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"state_remember");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,&fb,
        &blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);

    /*  Row at low: state 1 from the first 0x2d. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+2,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low);
    TASK_CHECK(st.state == 1 && st.signing_pc == 0);

    /*  Row at low+4: after 0x2d (1->0) and 0x2c (0->2) the
        state is 2 with signing at low+4. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+6,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+4);

    /*  Row at low+12: the inner restore brought back state 2
        with its saved signing pc, without rewinding. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+14,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+12);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+4);

    /*  Row at low+16: the outer restore brought back state 1. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+18,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+16);
    TASK_CHECK(st.state == 1 && st.signing_pc == 0);

    /*  Row at low+20: the trailing 0x2d leaves state 0. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+22,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+20);
    TASK_CHECK(st.state == 0 && st.signing_pc == 0);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_shared_cie(void)
{
    char path[512];
    Opened o;
    Fx f;
    unsigned cie = 0;
    unsigned char fi[16];
    unsigned flen = fde_shared(fi);
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0;
    Dwarf_Bool more = 0;

    fx_init(&f,0);
    cie = fx_cie(&f,"",4,-8,30,0,0);
    TASK_CHECK(cie != (unsigned)-1);
    fx_fde(&f,cie,0,0x1000,0x40,fi,flen);
    fx_fde(&f,cie,0,0x2000,0x40,fi,flen);
    tmp_name(path,sizeof(path),"state_shared");
    TASK_CHECK(fx_write(path,183,&f) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(o.nf == 2 && o.nc == 1);

    /*  Each FDE uses its own low pc but the same CIE. */
    TASK_CHECK(ra_state(&o,o.fdes[0],0x1008,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == 0x1004);
    TASK_CHECK(st.state == 2 && st.signing_pc == 0x1004);
    TASK_CHECK(ra_state(&o,o.fdes[1],0x2008,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == 0x2004);
    TASK_CHECK(st.state == 2 && st.signing_pc == 0x2004);
    /*  And back to the first FDE. */
    TASK_CHECK(ra_state(&o,o.fdes[0],0x1008,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(st.state == 2 && st.signing_pc == 0x1004);

    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_reverse_pc(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_main(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned ignored = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed cidx = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"state_reverse");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,&fb,
        &blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);

    /*  Query high rows first, then lower ones. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+30,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+28 && st.state == 2 &&
        st.signing_pc == low+24);
    TASK_CHECK(ra_state(&o,o.fdes[0],low+6,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+4 && st.state == 2 &&
        st.signing_pc == low+16);
    TASK_CHECK(ra_state(&o,o.fdes[0],low+22,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+20 && st.state == 0 &&
        st.signing_pc == 0);
    TASK_CHECK(ra_state(&o,o.fdes[0],low+30,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+28 && st.state == 2 &&
        st.signing_pc == low+24);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_mixed_queries(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_main(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned ignored = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed cidx = 0;
    Dwarf_Regtable_Entry3 rules[64];
    Dwarf_Regtable3 rt;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"state_mixed");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,&fb,
        &blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);

    /*  RA, then CFA, then register, then a full row, then RA
        again: all at the same pc, and the state must survive. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+6,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+16);
    {
        Dwarf_Small vt = 0;
        Dwarf_Unsigned rel = 0, reg = 0;
        Dwarf_Signed off = 0;
        Dwarf_Block blk;
        memset(&blk,0,sizeof(blk));
        TASK_CHECK(dwarf_get_fde_info_for_cfa_reg3_c(o.fdes[0],
            low+6,&vt,&rel,&reg,&off,&blk,&row,&more,&next,
            &o.err) == DW_DLV_OK);
        TASK_CHECK(reg == 31 && off == 16);
        memset(&blk,0,sizeof(blk));
        TASK_CHECK(dwarf_get_fde_info_for_reg3_c(o.fdes[0],30,
            low+6,&vt,&rel,&reg,&off,&blk,&row,&more,&next,
            &o.err) == DW_DLV_OK);
        TASK_CHECK(off == -16);
    }
    memset(&rt,0,sizeof(rt));
    memset(rules,0,sizeof(rules));
    rt.rt3_reg_table_size = 64;
    rt.rt3_rules = rules;
    TASK_CHECK(dwarf_get_fde_info_for_all_regs3_b(o.fdes[0],
        low+6,&rt,&row,&more,&next,&o.err) == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    /*  After the all-regs call the cached row is gone, but a new
        RA query must rebuild it exactly. */
    TASK_CHECK(ra_state(&o,o.fdes[0],low+6,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+4);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+16);
    TASK_CHECK(more == TRUE && next == low+20);

    /*  Same again for a different pc reached through the all-regs
        interface first. */
    memset(&rt,0,sizeof(rt));
    memset(rules,0,sizeof(rules));
    rt.rt3_reg_table_size = 64;
    rt.rt3_rules = rules;
    TASK_CHECK(dwarf_get_fde_info_for_all_regs3_b(o.fdes[0],
        low+30,&rt,&row,&more,&next,&o.err) == DW_DLV_OK);
    TASK_CHECK(ra_state(&o,o.fdes[0],low+30,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(row == low+28);
    TASK_CHECK(st.state == 2 && st.signing_pc == low+24);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_state_b_key(void)
{
    char path[512];
    Opened o;
    unsigned char instr[16];
    unsigned ilen = 0;
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0, low = 0, blen = 0;
    Dwarf_Bool more = 0;
    Dwarf_Unsigned ignored = 0;
    Dwarf_Small *fb = 0;
    Dwarf_Off cf = 0, df = 0;
    Dwarf_Signed cidx = 0;

    /*  A .debug_frame CIE whose augmentation is just "B". */
    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    instr[ilen++] = 0x2c;  /* enter state 2 at the low pc */
    tmp_name(path,sizeof(path),"state_bkey");
    TASK_CHECK(build_one(path,0,183,"B",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,&fb,
        &blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);
    TASK_CHECK(ra_state(&o,o.fdes[0],low,&st,&row,&more,
        &next) == DW_DLV_OK);
    TASK_CHECK(st.state == 2);
    TASK_CHECK(st.b_key == TRUE);
    close_fixture(&o);
    remove(path);

    /*  The zBRT epilogue in .eh_frame: the B consumes no data,
        so the R encoding remains in place and the low pc is
        read correctly.  The state is also the B key. */
    tmp_name(path,sizeof(path),"state_bkey_eh");
    {
        unsigned char fi[16];
        unsigned flen = 0;
        fi[flen++] = 0x2c;
        TASK_CHECK(build_one(path,1,183,"zBR",4,cie,clen,
            0x3000,0x20,fi,flen) == 0);
        TASK_CHECK(open_fixture(&o,path,1) == DW_DLV_OK);
        TASK_CHECK(dwarf_get_fde_range(o.fdes[0],&low,&ignored,
            &fb,&blen,&cf,&cidx,&df,&o.err) == DW_DLV_OK);
        TASK_CHECK(low == 0x3000);
        TASK_CHECK(ra_state(&o,o.fdes[0],low,&st,&row,&more,
            &next) == DW_DLV_OK);
        TASK_CHECK(st.state == 2 && st.b_key == TRUE);
        close_fixture(&o);
    }
    remove(path);
    return 0;
}

static int case_state_non_aarch64(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_non_aarch64(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Ra_State st;
    Dwarf_Addr row = 0, next = 0;
    Dwarf_Bool more = 0;
    Dwarf_Small vt = 0;
    Dwarf_Unsigned rel = 0, reg = 0;
    Dwarf_Signed off = 0;
    Dwarf_Block blk;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"state_x86");
    TASK_CHECK(build_one(path,0,62,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(o.nf == 1);
    memset(&st,0x5a,sizeof(st));
    TASK_CHECK(ra_state(&o,o.fdes[0],0x1002,&st,&row,&more,
        &next) == DW_DLV_NO_ENTRY);
    /*  The ordinary row model is unaffected, and 0x2d is a
        GNU_window_save no-op rather than a state change. */
    memset(&blk,0,sizeof(blk));
    TASK_CHECK(dwarf_get_fde_info_for_cfa_reg3_c(o.fdes[0],
        0x1002,&vt,&rel,&reg,&off,&blk,&row,&more,&next,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(reg == 31 && off == 16);
    memset(&blk,0,sizeof(blk));
    TASK_CHECK(dwarf_get_fde_info_for_reg3_c(o.fdes[0],30,
        0x1002,&vt,&rel,&reg,&off,&blk,&row,&more,&next,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(off == -16);
    close_fixture(&o);
    remove(path);
    return 0;
}

/*  ----------------------------------------------------------------- */
/*  Group Decode: expanded instructions and operand fields.           */
/*  ----------------------------------------------------------------- */

/*  The byte offsets of each instruction in the decode stream. */
static const unsigned char decode_stream[] = {
    0x04, 0xe8, 0x03, 0x00, 0x00, /* advance_loc4 1000 -> +4000 */
    0x2b, 0x02, 0xff, 0x7e,       /* set_ra_state 2, offset -129 */
    0x2c,                         /* negate 2 -> 0 */
    0x2d,                         /* negate 0 -> 1 */
    0x41,                         /* advance_loc 1 -> +4004 */
    0x2d,                         /* negate 1 -> 0 */
    0x2c,                         /* negate 0 -> 2, signing +4004 */
    0x0c, 0x1f, 0x10              /* def_cfa SP+16 */
};

static int case_decode_fields(void)
{
    char path[512];
    Opened o;
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Small *bytes = 0;
    Dwarf_Unsigned len = 0;
    Dwarf_Frame_Instr_Head head = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned ioff = 0, u0 = 0, u1 = 0, u2 = 0, ca = 0;
    Dwarf_Small op = 0;
    const char *fields = 0;
    Dwarf_Signed s0 = 0, s1 = 0, da = 0;
    Dwarf_Block blk;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"decode_fields");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x100,decode_stream,sizeof(decode_stream)) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_instr_bytes(o.fdes[0],&bytes,&len,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(dwarf_expand_frame_instructions(o.cies[0],bytes,
        len,&head,&count,&o.err) == DW_DLV_OK);
    /*  The eight real instructions plus the alignment nops that
        are part of the FDE instruction area. */
    TASK_CHECK(count >= 8);

    /*  0: advance_loc4 at offset 0. */
    TASK_CHECK(dwarf_get_frame_instruction_a(head,0,&ioff,&op,
        &fields,&u0,&u1,&u2,&s0,&s1,&ca,&da,&blk,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(ioff == 0 && op == DW_CFA_advance_loc4);

    /*  1: set_ra_state at offset 5; fields "us" with the raw
        state in u0 and the unfactored signed offset in s0. */
    memset(&blk,0,sizeof(blk));
    TASK_CHECK(dwarf_get_frame_instruction_a(head,1,&ioff,&op,
        &fields,&u0,&u1,&u2,&s0,&s1,&ca,&da,&blk,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(ioff == 5);
    TASK_CHECK(op == DW_CFA_AARCH64_set_ra_state);
    TASK_CHECK(!strcmp(fields,"us"));
    TASK_CHECK(u0 == 2);
    TASK_CHECK(s0 == -129);
    /*  The raw operand is returned: the code alignment factor
        is available separately and was not applied to s0. */
    TASK_CHECK(ca == 0);

    /*  2: negate_ra_state_with_pc at offset 9, no operands. */
    memset(&blk,0,sizeof(blk));
    TASK_CHECK(dwarf_get_frame_instruction_a(head,2,&ioff,&op,
        &fields,&u0,&u1,&u2,&s0,&s1,&ca,&da,&blk,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(ioff == 9);
    TASK_CHECK(op == DW_CFA_AARCH64_negate_ra_state_with_pc);
    TASK_CHECK(!strcmp(fields,""));
    TASK_CHECK(u0 == 0 && s0 == 0);

    /*  3: negate_ra_state (0x2d) at offset 10, no operands. */
    memset(&blk,0,sizeof(blk));
    TASK_CHECK(dwarf_get_frame_instruction_a(head,3,&ioff,&op,
        &fields,&u0,&u1,&u2,&s0,&s1,&ca,&da,&blk,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(ioff == 10);
    TASK_CHECK(op == 0x2d);
    TASK_CHECK(!strcmp(fields,""));
    TASK_CHECK(u0 == 0 && s0 == 0);

    /*  7: the trailing def_cfa must still be at the exact byte
        offset: 5 (advance) + 4 (set) + 1 + 1 + 1 + 1 + 1 = 14. */
    memset(&blk,0,sizeof(blk));
    TASK_CHECK(dwarf_get_frame_instruction_a(head,7,&ioff,&op,
        &fields,&u0,&u1,&u2,&s0,&s1,&ca,&da,&blk,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(ioff == 14);
    TASK_CHECK(op == DW_CFA_def_cfa);
    TASK_CHECK(u0 == 31 && u1 == 16);

    dwarf_dealloc_frame_instr_head(head);
    close_fixture(&o);
    remove(path);
    return 0;
}

static int case_decode_non_aarch64(void)
{
    char path[512];
    Opened o;
    unsigned char instr[64];
    unsigned ilen = seq_non_aarch64(instr);
    unsigned char cie[16];
    unsigned clen = 3;
    Dwarf_Small *bytes = 0;
    Dwarf_Unsigned len = 0;
    Dwarf_Frame_Instr_Head head = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned ioff = 0, u0 = 0, u1 = 0, u2 = 0, ca = 0;
    Dwarf_Small op = 0;
    const char *fields = 0;
    Dwarf_Signed s0 = 0, s1 = 0, da = 0;
    Dwarf_Block blk;
    Dwarf_Unsigned k = 0;
    int found_2d = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    tmp_name(path,sizeof(path),"decode_x86");
    TASK_CHECK(build_one(path,0,62,"",4,cie,clen,
        0x1000,0x40,instr,ilen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    TASK_CHECK(dwarf_get_fde_instr_bytes(o.fdes[0],&bytes,&len,
        &o.err) == DW_DLV_OK);
    TASK_CHECK(dwarf_expand_frame_instructions(o.cies[0],bytes,
        len,&head,&count,&o.err) == DW_DLV_OK);
    for (k = 0; k < count; ++k) {
        memset(&blk,0,sizeof(blk));
        TASK_CHECK(dwarf_get_frame_instruction_a(head,k,&ioff,&op,
            &fields,&u0,&u1,&u2,&s0,&s1,&ca,&da,&blk,
            &o.err) == DW_DLV_OK);
        if (op == 0x2d) {
            /*  Kept as GNU_window_save: no operands. */
            found_2d = 1;
            TASK_CHECK(!strcmp(fields,""));
            TASK_CHECK(u0 == 0 && s0 == 0);
        }
    }
    TASK_CHECK(found_2d == 1);
    dwarf_dealloc_frame_instr_head(head);
    close_fixture(&o);
    remove(path);
    return 0;
}

/*  A truncated LEB in set_ra_state follows the existing error
    path (DW_DLV_ERROR, not a crash or silent success).  The
    instructions are passed directly to
    dwarf_expand_frame_instructions() with a valid CIE, which is
    what that interface is for. */
static int case_decode_truncated_leb(void)
{
    char path[512];
    Opened o;
    unsigned char cie[16];
    unsigned clen = 3;
    unsigned char instr[8];
    unsigned ilen = 0;
    Dwarf_Frame_Instr_Head head = 0;
    Dwarf_Unsigned count = 0;
    int res = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    /*  0x2b then a ULEB with the continuation bit set and no
        following byte. */
    instr[ilen++] = 0x2b;
    instr[ilen++] = 0x82;
    tmp_name(path,sizeof(path),"decode_trunc");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,cie,clen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    res = dwarf_expand_frame_instructions(o.cies[0],instr,ilen,
        &head,&count,&o.err);
    TASK_CHECK(res == DW_DLV_ERROR);
    clear_err(&o);
    close_fixture(&o);
    remove(path);
    return 0;
}

/*  A reserved state value (greater than 2) is rejected through
    the existing frame decoding error path. */
static int case_decode_reserved_state(void)
{
    char path[512];
    Opened o;
    unsigned char cie[16];
    unsigned clen = 3;
    unsigned char instr[8];
    unsigned ilen = 0;
    Dwarf_Frame_Instr_Head head = 0;
    Dwarf_Unsigned count = 0;
    int res = 0;

    cie[0] = 0x0c; cie[1] = 31; cie[2] = 16;
    instr[ilen++] = 0x2b;
    instr[ilen++] = 0x03;  /* state 3: reserved */
    instr[ilen++] = 0x00;  /* offset 0 */
    tmp_name(path,sizeof(path),"decode_reserved");
    TASK_CHECK(build_one(path,0,183,"",4,cie,clen,
        0x1000,0x40,cie,clen) == 0);
    TASK_CHECK(open_fixture(&o,path,0) == DW_DLV_OK);
    res = dwarf_expand_frame_instructions(o.cies[0],instr,ilen,
        &head,&count,&o.err);
    TASK_CHECK(res == DW_DLV_ERROR);
    clear_err(&o);
    close_fixture(&o);
    remove(path);
    return 0;
}

/*  ----------------------------------------------------------------- */
/*  Case table.                                                       */
/*  ----------------------------------------------------------------- */

const struct task_case task_cases[] = {
    /* Rows */
    {"Rows","debug_frame_rows_and_ra",case_rows_debug_frame},
    {"Rows","eh_frame_augmentation_rows",case_rows_eh_frame},
    /* State */
    {"State","initial_state_zero",case_state_initial_zero},
    {"State","cie_initial_state_one",case_state_cie_initial_one},
    {"State","negative_set_offset",case_state_negative_offset},
    {"State","remember_restore",case_state_remember_restore},
    {"State","shared_cie_two_fdes",case_state_shared_cie},
    {"State","reverse_pc_queries",case_state_reverse_pc},
    {"State","mixed_row_queries",case_state_mixed_queries},
    {"State","b_key_selection",case_state_b_key},
    {"State","non_aarch64_no_entry",case_state_non_aarch64},
    /* Decode */
    {"Decode","set_ra_state_fields",case_decode_fields},
    {"Decode","non_aarch64_window_save",case_decode_non_aarch64},
    {"Decode","truncated_leb",case_decode_truncated_leb},
    {"Decode","reserved_state",case_decode_reserved_state}
};

const size_t task_case_count =
    sizeof(task_cases)/sizeof(task_cases[0]);
