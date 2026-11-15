/*
  Contract tests for the DWARF5 call site snapshot and tail call
  chain query interface in libdwarf.

  The tests build legal synthetic ELF objects holding several
  compilation units, lexical blocks, same-named functions in
  different CUs, register forwarding across several tail edges,
  a diamond shaped pair of tail exits, a cycle, an indirect
  call target, and unsupported or overflowing expressions.

  Only debug information is read. No instruction is executed and
  no process is inspected.

  Every case registers with task_support/check.h so the driver
  can report a per group case and check count.
*/

#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dwarf.h"
#include "libdwarf.h"
#include "../task_support/check.h"

/*  ---- growable byte buffer ---- */

struct buf {
    unsigned char *b;
    size_t n;
    size_t cap;
};

static void
buf_need(struct buf *b,size_t extra)
{
    if (b->n + extra <= b->cap) {
        return;
    }
    {
        size_t newcap = b->cap ? b->cap * 2 : 1024;

        while (newcap < b->n + extra) {
            newcap *= 2;
        }
        {
            unsigned char *fresh = (unsigned char *)realloc(b->b,
                newcap);

            if (!fresh) {
                fprintf(stderr,"out of memory\n");
                exit(2);
            }
            memset(fresh + b->cap,0,newcap - b->cap);
            b->b = fresh;
            b->cap = newcap;
        }
    }
}

static void
buf_put(struct buf *b,uint64_t v,unsigned n)
{
    buf_need(b,n);
    while (n--) {
        b->b[b->n++] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
}

static void
buf_patch(struct buf *b,size_t at,uint64_t v,unsigned n)
{
    while (n--) {
        b->b[at++] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
}

static void
buf_uleb(struct buf *b,uint64_t v)
{
    do {
        unsigned char c = (unsigned char)(v & 0x7f);

        v >>= 7;
        buf_put(b,c | (v?0x80:0),1);
    } while (v);
}

static void
buf_str(struct buf *b,const char *s)
{
    size_t len = strlen(s) + 1;

    buf_need(b,len);
    memcpy(b->b + b->n,s,len);
    b->n += len;
}

/*  ---- abbreviation table ---- */

struct abpair {
    unsigned attr;
    unsigned form;
};

static void
abbrev_entry(struct buf *ab,unsigned code,unsigned tag,
    unsigned children,const struct abpair *pairs)
{
    buf_uleb(ab,code);
    buf_uleb(ab,tag);
    buf_put(ab,children,1);
    for (;;) {
        buf_uleb(ab,pairs->attr);
        buf_uleb(ab,pairs->form);
        if (!pairs->attr) {
            break;
        }
        ++pairs;
    }
}

/*  ---- one compilation unit under construction ----

    The CU buffer starts with its 12 byte DWARF5 header, so
    recorded DIE offsets are CU relative including the header,
    exactly the offsets libdwarf reports for DW_FORM_ref4.
    Each DIE with children must be followed by one zero entry.
    A call_site_parameter has no children and needs no terminator.
*/

#define CU_MAX_DIES 64
#define CU_MAX_PATCHES 64
#define CU_ARENA 16384

/*  Symbolic function labels so a call can refer to a function DIE
    that is emitted later in the same or another CU.  */
enum flabel {
    FL_ORDCALL = 0,
    FL_CHAIN,
    FL_FB,
    FL_FC,
    FL_FD,
    FL_FE,
    FL_GATE,
    FL_CYCA,
    FL_CYCB,
    FL_CLONE,
    FL_FBC,
    FL_UNNAMED,
    FL_LOOSE,
    FL_FCC,
    FL_COUNT
};

struct refpatch {
    size_t pos;            /* byte position inside the CU */
    unsigned is_ref_addr;  /* 0: ref4 CU relative, 1: ref_addr */
    unsigned target_cu;
    unsigned target_label;
};

struct cuw {
    unsigned char b[CU_ARENA];
    size_t n;
    size_t die_off[CU_MAX_DIES];
    unsigned die_count;
    struct refpatch patches[CU_MAX_PATCHES];
    unsigned patch_count;
};

static void
cu_bytes(struct cuw *cu,const void *bytes,size_t len)
{
    if (cu->n + len > CU_ARENA) {
        fprintf(stderr,"CU overflow\n");
        exit(2);
    }
    memcpy(cu->b + cu->n,bytes,len);
    cu->n += len;
}

static void
cu_put(struct cuw *cu,uint64_t v,unsigned n)
{
    while (n--) {
        if (cu->n >= CU_ARENA) {
            fprintf(stderr,"CU overflow\n");
            exit(2);
        }
        cu->b[cu->n++] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
}

static void
cu_uleb(struct cuw *cu,uint64_t v)
{
    do {
        unsigned char c = (unsigned char)(v & 0x7f);

        v >>= 7;
        cu_put(cu,c | (v?0x80:0),1);
    } while (v);
}

static void
cu_begin(struct cuw *cu)
{
    memset(cu,0,sizeof(*cu));
    cu_put(cu,0,4);        /* unit_length, patched at finish */
    cu_put(cu,5,2);        /* version 5 */
    cu_put(cu,1,1);        /* DW_UT_compile */
    cu_put(cu,4,1);        /* address size */
    cu_put(cu,0,4);        /* abbrev offset */
}

static unsigned
cu_die(struct cuw *cu,unsigned code)
{
    unsigned index = cu->die_count++;

    if (index >= CU_MAX_DIES) {
        fprintf(stderr,"too many DIEs\n");
        exit(2);
    }
    cu->die_off[index] = cu->n;
    cu_uleb(cu,code);
    return index;
}

static void
cu_end_children(struct cuw *cu)
{
    cu_uleb(cu,0);
}

static void
cu_addr(struct cuw *cu,uint64_t value)
{
    cu_put(cu,value,4);
}

static void
cu_str(struct cuw *cu,const char *s)
{
    size_t len = strlen(s) + 1;

    cu_bytes(cu,s,len);
}

static void
cu_flag(struct cuw *cu,unsigned value)
{
    cu_put(cu,value?1:0,1);
}

static void
cu_expr(struct cuw *cu,const unsigned char *bytes,size_t len)
{
    cu_uleb(cu,len);
    cu_bytes(cu,bytes,len);
}

static void
cu_ref(struct cuw *cu,unsigned target_cu,unsigned target_label,
    unsigned is_ref_addr)
{
    struct refpatch *p = 0;

    if (cu->patch_count >= CU_MAX_PATCHES) {
        fprintf(stderr,"too many ref patches\n");
        exit(2);
    }
    p = &cu->patches[cu->patch_count++];
    p->pos = cu->n;
    p->is_ref_addr = is_ref_addr;
    p->target_cu = target_cu;
    p->target_label = target_label;
    cu_put(cu,0xffffffff,4);
}

/*  ---- whole object under construction ---- */

#define MAX_CUS 6

struct object {
    struct buf abbrev;
    struct buf info;
    struct buf names;
    struct buf file;
    struct cuw *cus[MAX_CUS];
    unsigned cu_count;
    size_t label_die[MAX_CUS][FL_COUNT];
};

static struct cuw g_cu_storage[MAX_CUS];

static struct cuw *
object_cu(struct object *obj)
{
    struct cuw *cu = 0;

    if (obj->cu_count >= MAX_CUS) {
        fprintf(stderr,"too many CUs\n");
        exit(2);
    }
    cu = &g_cu_storage[obj->cu_count++];
    cu_begin(cu);
    obj->cus[obj->cu_count - 1] = cu;
    return cu;
}

/*  Open a function DIE and remember it under a label.  */
static void
cu_function(struct object *obj,struct cuw *cu,unsigned code,
    enum flabel label)
{
    unsigned die = cu_die(cu,code);

    obj->label_die[obj->cu_count - 1][label] = cu->die_off[die];
}

static void
object_finish(struct object *obj)
{
    size_t base[MAX_CUS];
    size_t total = 0;
    unsigned i = 0;

    for (i = 0; i < obj->cu_count; ++i) {
        struct cuw *cu = obj->cus[i];

        /*  unit_length excludes its own 4 bytes.  */
        {
            size_t j = 0;
            uint64_t length = (uint64_t)(cu->n - 4);

            for (j = 0; j < 4; ++j) {
                cu->b[j] = (unsigned char)((length >> (j*8)) & 0xff);
            }
        }
        base[i] = total;
        total += cu->n;
    }
    buf_need(&obj->info,total);
    for (i = 0; i < obj->cu_count; ++i) {
        struct cuw *cu = obj->cus[i];
        unsigned pi = 0;

        memcpy(obj->info.b + base[i],cu->b,cu->n);
        for (pi = 0; pi < cu->patch_count; ++pi) {
            struct refpatch *p = &cu->patches[pi];
            uint64_t target_global = (uint64_t)(base[p->target_cu] +
                obj->label_die[p->target_cu][p->target_label]);
            uint64_t value = p->is_ref_addr ? target_global :
                target_global - base[i];

            buf_patch(&obj->info,base[i] + p->pos,value,4);
        }
    }
    obj->info.n = total;
}

static void
elf_section(struct buf *file,unsigned name,unsigned type,
    uint64_t flags,uint64_t addr,uint64_t off,uint64_t size)
{
    buf_put(file,name,4);
    buf_put(file,type,4);
    buf_put(file,flags,8);
    buf_put(file,addr,8);
    buf_put(file,off,8);
    buf_put(file,size,8);
    buf_put(file,0,4);
    buf_put(file,0,4);
    buf_put(file,1,8);
    buf_put(file,0,8);
}

static void
object_elf(struct object *obj)
{
    struct buf *file = &obj->file;
    size_t off_info = 0;
    size_t off_ab = 0;
    size_t off_names = 0;
    size_t off_text = 0;
    size_t sht = 0;
    unsigned name_info = 0;
    unsigned name_ab = 0;
    unsigned name_names = 0;
    unsigned name_text = 0;
    unsigned i = 0;

    buf_put(&obj->names,0,1);       /* empty string at offset 0 */
    name_info = (unsigned)obj->names.n;
    buf_str(&obj->names,".debug_info");
    name_ab = (unsigned)obj->names.n;
    buf_str(&obj->names,".debug_abbrev");
    name_names = (unsigned)obj->names.n;
    buf_str(&obj->names,".shstrtab");
    name_text = (unsigned)obj->names.n;
    buf_str(&obj->names,".text");

    buf_put(file,0x464c457f,4);
    buf_put(file,2,1);            /* ELFCLASS64 */
    buf_put(file,1,1);            /* little endian */
    buf_put(file,1,1);            /* EV_CURRENT */
    buf_put(file,0,1);            /* ELFOSABI_NONE */
    for (i = 0; i < 8; ++i) {
        buf_put(file,0,1);
    }
    buf_put(file,2,2);            /* ET_EXEC */
    buf_put(file,62,2);           /* EM_X86_64 */
    buf_put(file,1,4);
    buf_put(file,0x1000,8);
    buf_put(file,0,8);
    buf_put(file,0,8);
    buf_put(file,0,4);
    buf_put(file,64,2);
    buf_put(file,0,2);
    buf_put(file,0,2);
    buf_put(file,64,2);
    buf_put(file,5,2);            /* section header count */
    buf_put(file,3,2);            /* .shstrtab index */

    off_info = file->n;
    buf_need(file,obj->info.n);
    memcpy(file->b + file->n,obj->info.b,obj->info.n);
    file->n += obj->info.n;
    off_ab = file->n;
    buf_need(file,obj->abbrev.n);
    memcpy(file->b + file->n,obj->abbrev.b,obj->abbrev.n);
    file->n += obj->abbrev.n;
    off_names = file->n;
    buf_need(file,obj->names.n);
    memcpy(file->b + file->n,obj->names.b,obj->names.n);
    file->n += obj->names.n;
    off_text = file->n;
    for (i = 0; i < 64; ++i) {
        buf_put(file,0xc3,1);
    }
    while (file->n % 8) {
        buf_put(file,0,1);
    }
    sht = file->n;
    elf_section(file,0,0,0,0,0,0);
    elf_section(file,name_info,1,0,0,off_info,obj->info.n);
    elf_section(file,name_ab,1,0,0,off_ab,obj->abbrev.n);
    elf_section(file,name_names,3,0,0,off_names,obj->names.n);
    elf_section(file,name_text,1,6,0x1000,off_text,64);
    buf_patch(file,40,sht,8);
}

/*  ---- expression bytes used by the fixture ---- */

static const unsigned char expr_reg5[] = {DW_OP_reg5};
static const unsigned char expr_reg6[] = {DW_OP_reg6};
static const unsigned char expr_reg7[] = {DW_OP_reg7};
static const unsigned char expr_reg8[] = {DW_OP_reg8};
static const unsigned char expr_reg9[] = {DW_OP_reg9};
static const unsigned char expr_reg10[] = {DW_OP_reg10};
static const unsigned char expr_reg11[] = {DW_OP_reg11};
static const unsigned char expr_reg12[] = {DW_OP_reg12};
static const unsigned char expr_const5[] = {DW_OP_constu,5};
static const unsigned char expr_const7[] = {DW_OP_constu,7};
static const unsigned char expr_entry_r5_plus3[] = {
    DW_OP_entry_value,1,DW_OP_reg5,DW_OP_constu,3,DW_OP_plus,
    DW_OP_stack_value};
static const unsigned char expr_entry_r5_plus100[] = {
    DW_OP_entry_value,1,DW_OP_reg5,DW_OP_constu,100,
    DW_OP_plus,DW_OP_stack_value};
static const unsigned char expr_entry_r6_plus1[] = {
    DW_OP_entry_value,1,DW_OP_reg6,DW_OP_constu,1,DW_OP_plus,
    DW_OP_stack_value};
static const unsigned char expr_entry_r12_plus2[] = {
    DW_OP_entry_value,1,DW_OP_reg12,DW_OP_constu,2,DW_OP_plus,
    DW_OP_stack_value};
static const unsigned char expr_entry_r6_mul100[] = {
    DW_OP_entry_value,1,DW_OP_reg6,DW_OP_constu,100,DW_OP_mul,
    DW_OP_stack_value};
static const unsigned char expr_overflow[] = {
    DW_OP_constu,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,
    DW_OP_constu,1,DW_OP_plus,DW_OP_stack_value};
static const unsigned char expr_consts_minus5_plus2[] = {
    DW_OP_consts,0x7b,DW_OP_constu,2,DW_OP_plus,
    DW_OP_stack_value};
static const unsigned char expr_entry_r5_minus1[] = {
    DW_OP_entry_value,1,DW_OP_reg5,DW_OP_constu,1,DW_OP_minus,
    DW_OP_stack_value};
static const unsigned char expr_target_reg1[] = {DW_OP_reg1};
static const unsigned char expr_regx40[] = {DW_OP_regx,40};

/*  ---- fixture construction ---- */

/*  The fixture functions are looked up by name at test time.  The
    two same-named pairs are deliberately distinct DIEs: "ordcall"
    exists once in CU0 and once in CU2 at the same CU relative
    offset, and "chain" exists once in CU0 and once in CU1.  */

/*
  Abbreviations used below.

  0x01 compile_unit   name(str) low_pc(addr) stmt_list(data4)
  0x02 subprogram     name(str) low_pc(addr) high_pc(data4)
  0x03 subprogram     as 0x02 plus DW_AT_call_all_tail_calls(flag)
  0x04 lexical_block  (no attributes)
  0x05 call_site      return_pc(addr) origin(ref4)
  0x06 call_site      return_pc(addr) tail(flag) origin(ref4)
  0x07 call_site      return_pc(addr) tail(flag) origin(ref_addr)
  0x08 call_site      return_pc(addr) origin(ref_addr)
  0x09 call_site_param location(exprloc) value(exprloc)
  0x0a call_site_param location(exprloc)
  0x0b call_site      return_pc(addr) target(exprloc)
  0x0c subprogram     (no attributes)
*/

static void
build_abbrev_table(struct object *obj)
{
    static const struct abpair cu_ab[] = {
        {DW_AT_name,DW_FORM_string},
        {DW_AT_low_pc,DW_FORM_addr},
        {DW_AT_stmt_list,DW_FORM_data4},
        {0,0}};
    static const struct abpair fn_ab[] = {
        {DW_AT_name,DW_FORM_string},
        {DW_AT_low_pc,DW_FORM_addr},
        {DW_AT_high_pc,DW_FORM_data4},
        {0,0}};
    static const struct abpair fn_tail_ab[] = {
        {DW_AT_name,DW_FORM_string},
        {DW_AT_low_pc,DW_FORM_addr},
        {DW_AT_high_pc,DW_FORM_data4},
        {DW_AT_call_all_tail_calls,DW_FORM_flag_present},
        {0,0}};
    static const struct abpair block_ab[] = {{0,0}};
    static const struct abpair call_ref4_ab[] = {
        {DW_AT_call_return_pc,DW_FORM_addr},
        {DW_AT_call_origin,DW_FORM_ref4},
        {0,0}};
    static const struct abpair call_ref4_tail_ab[] = {
        {DW_AT_call_return_pc,DW_FORM_addr},
        {DW_AT_call_tail_call,DW_FORM_flag},
        {DW_AT_call_origin,DW_FORM_ref4},
        {0,0}};
    static const struct abpair call_refaddr_tail_ab[] = {
        {DW_AT_call_return_pc,DW_FORM_addr},
        {DW_AT_call_tail_call,DW_FORM_flag},
        {DW_AT_call_origin,DW_FORM_ref_addr},
        {0,0}};
    static const struct abpair call_refaddr_ab[] = {
        {DW_AT_call_return_pc,DW_FORM_addr},
        {DW_AT_call_origin,DW_FORM_ref_addr},
        {0,0}};
    static const struct abpair csp_ab[] = {
        {DW_AT_location,DW_FORM_exprloc},
        {DW_AT_call_value,DW_FORM_exprloc},
        {0,0}};
    static const struct abpair csp_novalue_ab[] = {
        {DW_AT_location,DW_FORM_exprloc},
        {0,0}};
    static const struct abpair target_ab[] = {
        {DW_AT_call_return_pc,DW_FORM_addr},
        {DW_AT_call_target,DW_FORM_exprloc},
        {0,0}};
    static const struct abpair unnamed_fn_ab[] = {{0,0}};

    abbrev_entry(&obj->abbrev,0x01,DW_TAG_compile_unit,1,cu_ab);
    abbrev_entry(&obj->abbrev,0x02,DW_TAG_subprogram,1,fn_ab);
    abbrev_entry(&obj->abbrev,0x03,DW_TAG_subprogram,1,fn_tail_ab);
    abbrev_entry(&obj->abbrev,0x04,DW_TAG_lexical_block,1,block_ab);
    abbrev_entry(&obj->abbrev,0x05,DW_TAG_call_site,1,call_ref4_ab);
    abbrev_entry(&obj->abbrev,0x06,DW_TAG_call_site,1,
        call_ref4_tail_ab);
    abbrev_entry(&obj->abbrev,0x07,DW_TAG_call_site,1,
        call_refaddr_tail_ab);
    abbrev_entry(&obj->abbrev,0x08,DW_TAG_call_site,1,
        call_refaddr_ab);
    abbrev_entry(&obj->abbrev,0x09,DW_TAG_call_site_parameter,0,
        csp_ab);
    abbrev_entry(&obj->abbrev,0x0a,DW_TAG_call_site_parameter,0,
        csp_novalue_ab);
    abbrev_entry(&obj->abbrev,0x0b,DW_TAG_call_site,1,target_ab);
    abbrev_entry(&obj->abbrev,0x0c,DW_TAG_subprogram,1,unnamed_fn_ab);
    buf_put(&obj->abbrev,0,1);
}

static void
build_cu0(struct object *obj)
{
    struct cuw *cu = object_cu(obj);
    const unsigned cu_index = 0;

    cu_die(cu,0x01);
    cu_str(cu,"fixture-a0.c");
    cu_addr(cu,0x1000);
    cu_addr(cu,0);

    /*  Ordinary caller with a lexical block holding three calls.  */
    cu_function(obj,cu,0x02,FL_ORDCALL);
    cu_str(cu,"ordcall");
    cu_addr(cu,0x1000);
    cu_addr(cu,0x40);
    cu_die(cu,0x04);                    /* lexical block */
    cu_die(cu,0x05);                    /* 0x100 -> chain */
    cu_addr(cu,0x100);
    cu_ref(cu,cu_index,FL_CHAIN,0);
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg5,sizeof(expr_reg5));
    cu_expr(cu,expr_const7,sizeof(expr_const7));
    cu_end_children(cu);                /* end call_site 0x100 */
    cu_die(cu,0x05);                    /* 0x101 -> chain */
    cu_addr(cu,0x101);
    cu_ref(cu,cu_index,FL_CHAIN,0);
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg5,sizeof(expr_reg5));
    cu_expr(cu,expr_const7,sizeof(expr_const7));
    cu_end_children(cu);                /* end call_site 0x101 */
    cu_die(cu,0x05);                    /* 0x101 -> fd */
    cu_addr(cu,0x101);
    cu_ref(cu,cu_index,FL_FD,0);
    cu_die(cu,0x09);
    cu_expr(cu,expr_regx40,sizeof(expr_regx40));
    cu_expr(cu,expr_const5,sizeof(expr_const5));
    cu_end_children(cu);                /* end call_site 0x101 fd */
    cu_end_children(cu);                /* end lexical block */
    cu_end_children(cu);                /* end ordcall */

    /*  Forwarding function with two tail exits: the diamond.  */
    cu_function(obj,cu,0x02,FL_CHAIN);
    cu_str(cu,"chain");
    cu_addr(cu,0x1100);
    cu_addr(cu,0x80);
    cu_die(cu,0x07);                    /* 0x200 tail -> fb */
    cu_addr(cu,0x200);
    cu_flag(cu,1);
    cu_ref(cu,cu_index,FL_FB,1);
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg6,sizeof(expr_reg6));
    cu_expr(cu,expr_entry_r5_plus3,sizeof(expr_entry_r5_plus3));
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg8,sizeof(expr_reg8));
    cu_expr(cu,expr_overflow,sizeof(expr_overflow));
    cu_end_children(cu);                /* end call_site 0x200 */
    cu_die(cu,0x07);                    /* 0x201 tail -> fd */
    cu_addr(cu,0x201);
    cu_flag(cu,1);
    cu_ref(cu,cu_index,FL_FD,1);
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg6,sizeof(expr_reg6));
    cu_expr(cu,expr_entry_r5_plus100,
        sizeof(expr_entry_r5_plus100));
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg9,sizeof(expr_reg9));
    cu_expr(cu,expr_consts_minus5_plus2,
        sizeof(expr_consts_minus5_plus2));
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg10,sizeof(expr_reg10));
    cu_expr(cu,expr_entry_r5_minus1,
        sizeof(expr_entry_r5_minus1));
    cu_end_children(cu);                /* end call_site 0x201 */
    cu_end_children(cu);                /* end chain */

    /*  Complete forwarding function.  */
    cu_function(obj,cu,0x03,FL_FB);
    cu_str(cu,"fb");
    cu_addr(cu,0x1200);
    cu_addr(cu,0x40);
    cu_die(cu,0x07);                    /* 0x300 tail -> fc */
    cu_addr(cu,0x300);
    cu_flag(cu,1);
    cu_ref(cu,cu_index,FL_FC,1);
    cu_die(cu,0x09);                    /* two hop entry(r6)+1 */
    cu_expr(cu,expr_reg7,sizeof(expr_reg7));
    cu_expr(cu,expr_entry_r6_plus1,
        sizeof(expr_entry_r6_plus1));
    cu_die(cu,0x09);                    /* symbolic entry(r12) */
    cu_expr(cu,expr_reg5,sizeof(expr_reg5));
    cu_expr(cu,expr_entry_r12_plus2,
        sizeof(expr_entry_r12_plus2));
    cu_die(cu,0x09);                    /* unsupported multiply */
    cu_expr(cu,expr_reg9,sizeof(expr_reg9));
    cu_expr(cu,expr_entry_r6_mul100,
        sizeof(expr_entry_r6_mul100));
    cu_die(cu,0x0a);                    /* location only */
    cu_expr(cu,expr_reg11,sizeof(expr_reg11));
    cu_end_children(cu);                /* end call_site 0x300 */
    cu_end_children(cu);                /* end fb */

    /*  Terminal function: complete, no tail exits.  */
    cu_function(obj,cu,0x03,FL_FC);
    cu_str(cu,"fc");
    cu_addr(cu,0x1300);
    cu_addr(cu,0x20);
    cu_end_children(cu);

    /*  Complete function with one tail exit to fc.  */
    cu_function(obj,cu,0x03,FL_FD);
    cu_str(cu,"fd");
    cu_addr(cu,0x1400);
    cu_addr(cu,0x20);
    cu_die(cu,0x07);                    /* 0x400 tail -> fc */
    cu_addr(cu,0x400);
    cu_flag(cu,1);
    cu_ref(cu,cu_index,FL_FC,1);
    cu_end_children(cu);                /* end call_site 0x400 */
    cu_end_children(cu);                /* end fd */

    /*  Function with no completeness declaration.  */
    cu_function(obj,cu,0x02,FL_FE);
    cu_str(cu,"fe");
    cu_addr(cu,0x1500);
    cu_addr(cu,0x20);
    cu_die(cu,0x07);                    /* 0x500 tail -> fb */
    cu_addr(cu,0x500);
    cu_flag(cu,1);
    cu_ref(cu,cu_index,FL_FB,1);
    cu_end_children(cu);                /* end call_site 0x500 */
    cu_end_children(cu);                /* end fe */

    /*  Gate: ordinary calls into fe and into CU2 loose.  */
    cu_function(obj,cu,0x02,FL_GATE);
    cu_str(cu,"gate");
    cu_addr(cu,0x1600);
    cu_addr(cu,0x20);
    cu_die(cu,0x08);                    /* 0x520 ordinary -> fe */
    cu_addr(cu,0x520);
    cu_ref(cu,cu_index,FL_FE,1);
    cu_die(cu,0x09);
    cu_expr(cu,expr_reg12,sizeof(expr_reg12));
    cu_expr(cu,expr_const5,sizeof(expr_const5));
    cu_end_children(cu);                /* end call_site 0x520 */
    cu_die(cu,0x08);                    /* 0x521 ordinary -> loose */
    cu_addr(cu,0x521);
    cu_ref(cu,2,FL_LOOSE,1);
    cu_end_children(cu);                /* end call_site 0x521 */
    cu_end_children(cu);                /* end gate */
    cu_end_children(cu);                /* end CU0 */
}

static void
build_cu1(struct object *obj)
{
    struct cuw *cu = object_cu(obj);
    const unsigned cu_index = 1;

    cu_die(cu,0x01);
    cu_str(cu,"fixture-b1.c");
    cu_addr(cu,0x2000);
    cu_addr(cu,0);

    /*  cyca: ordinary call into cycb.  */
    cu_function(obj,cu,0x02,FL_CYCA);
    cu_str(cu,"cyca");
    cu_addr(cu,0x2100);
    cu_addr(cu,0x20);
    cu_die(cu,0x08);                    /* 0x600 ordinary -> cycb */
    cu_addr(cu,0x600);
    cu_ref(cu,cu_index,FL_CYCB,1);
    cu_end_children(cu);                /* end call_site 0x600 */
    cu_end_children(cu);                /* end cyca */

    /*  cycb: tail call back into cyca.  */
    cu_function(obj,cu,0x03,FL_CYCB);
    cu_str(cu,"cycb");
    cu_addr(cu,0x2200);
    cu_addr(cu,0x20);
    cu_die(cu,0x07);                    /* 0x610 tail -> cyca */
    cu_addr(cu,0x610);
    cu_flag(cu,1);
    cu_ref(cu,cu_index,FL_CYCA,1);
    cu_end_children(cu);                /* end call_site 0x610 */
    cu_end_children(cu);                /* end cycb */

    /*  Clone named "chain": same name as CU0 chain, another CU.
        Its ordinary call targets CU0 fb across CUs.  */
    cu_function(obj,cu,0x02,FL_CLONE);
    cu_str(cu,"chain");
    cu_addr(cu,0x2300);
    cu_addr(cu,0x40);
    cu_die(cu,0x08);                    /* 0x609 ordinary -> CU0 fb */
    cu_addr(cu,0x609);
    cu_ref(cu,0,FL_FB,1);
    cu_end_children(cu);                /* end call_site 0x609 */
    cu_end_children(cu);                /* end clone */

    /*  fbc: complete terminal function in CU1.  */
    cu_function(obj,cu,0x03,FL_FBC);
    cu_str(cu,"fbc");
    cu_addr(cu,0x2400);
    cu_addr(cu,0x20);
    cu_end_children(cu);

    /*  Unnamed function with an indirect and a direct call.  */
    cu_function(obj,cu,0x0c,FL_UNNAMED);
    cu_die(cu,0x0b);                    /* 0x700 indirect target */
    cu_addr(cu,0x700);
    cu_expr(cu,expr_target_reg1,sizeof(expr_target_reg1));
    cu_end_children(cu);                /* end call_site 0x700 */
    cu_die(cu,0x08);                    /* 0x701 ordinary -> fbc */
    cu_addr(cu,0x701);
    cu_ref(cu,cu_index,FL_FBC,1);
    cu_end_children(cu);                /* end call_site 0x701 */
    cu_end_children(cu);                /* end unnamed */
    cu_end_children(cu);                /* end CU1 */
}

static void
build_cu2(struct object *obj)
{
    struct cuw *cu = object_cu(obj);
    const unsigned cu_index = 2;

    /*  The compile unit name has the same encoded length as CU0's
        so the first subprogram DIE lands at the same CU relative
        offset as CU0 ordcall.  */
    cu_die(cu,0x01);
    cu_str(cu,"fixture-c0.c");
    cu_addr(cu,0x3000);
    cu_addr(cu,0);

    /*  Same name and same CU relative DIE offset as CU0 ordcall. */
    cu_function(obj,cu,0x02,FL_ORDCALL);
    cu_str(cu,"ordcall");
    cu_addr(cu,0x3100);
    cu_addr(cu,0x40);
    cu_die(cu,0x05);                    /* 0x100 ordinary -> fcc */
    cu_addr(cu,0x100);
    cu_ref(cu,cu_index,FL_FCC,0);
    cu_end_children(cu);                /* end call_site 0x100 */
    cu_end_children(cu);                /* end ordcall */

    /*  No completeness, no tail exits.  */
    cu_function(obj,cu,0x02,FL_LOOSE);
    cu_str(cu,"loose");
    cu_addr(cu,0x3200);
    cu_addr(cu,0x20);
    cu_end_children(cu);

    /*  Complete terminal function.  */
    cu_function(obj,cu,0x03,FL_FCC);
    cu_str(cu,"fcc");
    cu_addr(cu,0x3300);
    cu_addr(cu,0x20);
    cu_end_children(cu);
    cu_end_children(cu);                /* end CU2 */
}

static char g_fixture_path[600];
static int g_fixture_ready = 0;

static int
write_fixture(void)
{
    struct object obj;
    FILE *out = 0;
    const char *tmpdir = getenv("TMPDIR");

    memset(&obj,0,sizeof(obj));
    build_abbrev_table(&obj);
    build_cu0(&obj);
    build_cu1(&obj);
    build_cu2(&obj);
    object_finish(&obj);
    object_elf(&obj);

    if (!tmpdir || !tmpdir[0]) {
        tmpdir = ".";
    }
    snprintf(g_fixture_path,sizeof(g_fixture_path),
        "%s/libdwarf-callsite-%ld.elf",tmpdir,(long)getpid());
    out = fopen(g_fixture_path,"wb");
    if (!out) {
        fprintf(stderr,"cannot write fixture at %s\n",
            g_fixture_path);
        return 0;
    }
    if (fwrite(obj.file.b,1,obj.file.n,out) != obj.file.n) {
        fclose(out);
        return 0;
    }
    if (fclose(out)) {
        return 0;
    }
    free(obj.abbrev.b);
    free(obj.info.b);
    free(obj.names.b);
    free(obj.file.b);
    g_fixture_ready = 1;
    return 1;
}

static int
ensure_fixture(void)
{
    if (g_fixture_ready) {
        return 1;
    }
    return write_fixture();
}

static int
open_fixture(Dwarf_Debug *dbg)
{
    Dwarf_Error error = 0;

    if (!ensure_fixture()) {
        return 0;
    }
    if (dwarf_init_path(g_fixture_path,0,0,DW_GROUPNUMBER_ANY,
        0,0,dbg,&error) != DW_DLV_OK) {
        if (error) {
            dwarf_dealloc_error(0,error);
        }
        return 0;
    }
    return 1;
}

/*  ---- lookup helpers ---- */

static int
find_fn_nth(Dwarf_Callsite_Snapshot snap,const char *name,
    unsigned nth,Dwarf_Callsite_Die_Identity *id)
{
    Dwarf_Error error = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned i = 0;
    unsigned seen = 0;

    if (dwarf_callsite_snapshot_function_count(snap,&count,
        &error) != DW_DLV_OK) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        Dwarf_Callsite_Die_Identity cur;
        const char *cur_name = 0;

        memset(&cur,0,sizeof(cur));
        if (dwarf_callsite_snapshot_function_identity(snap,i,&cur,
            &error) != DW_DLV_OK) {
            continue;
        }
        if (dwarf_callsite_function_name(snap,&cur,&cur_name,
            &error) != DW_DLV_OK) {
            continue;
        }
        if (!cur_name || strcmp(cur_name,name)) {
            continue;
        }
        if (seen++ != nth) {
            continue;
        }
        *id = cur;
        return 1;
    }
    return 0;
}

static int
find_unnamed(Dwarf_Callsite_Snapshot snap,
    Dwarf_Callsite_Die_Identity *id)
{
    Dwarf_Error error = 0;
    Dwarf_Unsigned count = 0;
    Dwarf_Unsigned i = 0;

    if (dwarf_callsite_snapshot_function_count(snap,&count,
        &error) != DW_DLV_OK) {
        return 0;
    }
    for (i = 0; i < count; ++i) {
        Dwarf_Callsite_Die_Identity cur;
        const char *name = 0;

        memset(&cur,0,sizeof(cur));
        if (dwarf_callsite_snapshot_function_identity(snap,i,&cur,
            &error) != DW_DLV_OK) {
            continue;
        }
        if (dwarf_callsite_function_name(snap,&cur,&name,
            &error) != DW_DLV_OK) {
            continue;
        }
        if (!name) {
            *id = cur;
            return 1;
        }
    }
    return 0;
}

static Dwarf_Callsite_Query
query_fn(Dwarf_Callsite_Snapshot snap,
    const Dwarf_Callsite_Die_Identity *id,Dwarf_Addr pc)
{
    Dwarf_Error error = 0;
    Dwarf_Callsite_Query q = 0;

    if (dwarf_callsite_query(snap,id,pc,&q,&error) != DW_DLV_OK) {
        if (error) {
            dwarf_dealloc_error(0,error);
        }
        return 0;
    }
    return q;
}

static int
bytes_equal(const unsigned char *a,const unsigned char *b,size_t len)
{
    if (!len) {
        return 1;
    }
    return a && b && !memcmp(a,b,len);
}

static int
same_identity(const Dwarf_Callsite_Die_Identity *a,
    const Dwarf_Callsite_Die_Identity *b)
{
    return a->dci_cu_header_offset == b->dci_cu_header_offset &&
        a->dci_die_offset == b->dci_die_offset;
}

static unsigned g_print_lines = 0;

static void
count_line(void *user,const char *line)
{
    (void)user;
    if (line && line[0]) {
        ++g_print_lines;
    }
}

/*  ==================================================================
    Group identity
    ================================================================== */

static int
case_identity_names_and_offsets(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord0;
    Dwarf_Callsite_Die_Identity ord2;
    Dwarf_Callsite_Die_Identity chain0;
    Dwarf_Callsite_Die_Identity chain1;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    /*  Two functions named "ordcall" in different CUs.  */
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord0));
    TASK_CHECK(find_fn_nth(snap,"ordcall",1,&ord2));
    TASK_CHECK(!same_identity(&ord0,&ord2));
    TASK_CHECK(ord0.dci_cu_header_offset !=
        ord2.dci_cu_header_offset);
    TASK_CHECK(ord0.dci_die_offset != ord2.dci_die_offset);
    /*  Two functions named "chain" in different CUs.  */
    TASK_CHECK(find_fn_nth(snap,"chain",0,&chain0));
    TASK_CHECK(find_fn_nth(snap,"chain",1,&chain1));
    TASK_CHECK(!same_identity(&chain0,&chain1));
    /*  The two ordcall DIEs share a CU relative offset but are
        still distinct DIEs.  */
    {
        Dwarf_Die die0 = 0;
        Dwarf_Die die2 = 0;
        Dwarf_Off local0 = 0;
        Dwarf_Off local2 = 0;

        TASK_CHECK(dwarf_offdie_b(dbg,ord0.dci_die_offset,1,
            &die0,&error) == DW_DLV_OK);
        TASK_CHECK(dwarf_offdie_b(dbg,ord2.dci_die_offset,1,
            &die2,&error) == DW_DLV_OK);
        TASK_CHECK(dwarf_die_CU_offset(die0,&local0,&error) ==
            DW_DLV_OK);
        TASK_CHECK(dwarf_die_CU_offset(die2,&local2,&error) ==
            DW_DLV_OK);
        TASK_CHECK(local0 == local2);
        TASK_CHECK(ord0.dci_die_offset != ord2.dci_die_offset);
        dwarf_dealloc_die(die0);
        dwarf_dealloc_die(die2);
    }
    /*  Each identity yields only its own calls.  */
    {
        Dwarf_Callsite_Query q0 = query_fn(snap,&ord0,0x100);
        Dwarf_Callsite_Query q2 = query_fn(snap,&ord2,0x100);
        Dwarf_Unsigned n0 = 0;
        Dwarf_Unsigned n2 = 0;
        Dwarf_Callsite_Call c0;
        Dwarf_Callsite_Call c2;

        TASK_CHECK(q0 != 0);
        TASK_CHECK(q2 != 0);
        TASK_CHECK(dwarf_callsite_query_call_count(q0,&n0,&error) ==
            DW_DLV_OK);
        TASK_CHECK(dwarf_callsite_query_call_count(q2,&n2,&error) ==
            DW_DLV_OK);
        TASK_CHECK(n0 == 1);
        TASK_CHECK(n2 == 1);
        memset(&c0,0,sizeof(c0));
        memset(&c2,0,sizeof(c2));
        TASK_CHECK(dwarf_callsite_query_call(q0,0,&c0,&error) ==
            DW_DLV_OK);
        TASK_CHECK(dwarf_callsite_query_call(q2,0,&c2,&error) ==
            DW_DLV_OK);
        TASK_CHECK(c0.cscall_call_site_offset !=
            c2.cscall_call_site_offset);
        TASK_CHECK(!same_identity(&c0.cscall_origin,
            &c2.cscall_origin));
        dwarf_callsite_query_destroy(q0);
        dwarf_callsite_query_destroy(q2);
    }
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

static int
case_identity_call_ownership(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord;
    Dwarf_Callsite_Die_Identity chain;
    Dwarf_Callsite_Die_Identity fb;
    Dwarf_Callsite_Die_Identity gate;
    Dwarf_Callsite_Die_Identity unnamed;
    Dwarf_Unsigned count = 0;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord));
    TASK_CHECK(find_fn_nth(snap,"chain",0,&chain));
    TASK_CHECK(find_fn_nth(snap,"fb",0,&fb));
    TASK_CHECK(find_fn_nth(snap,"gate",0,&gate));
    TASK_CHECK(find_unnamed(snap,&unnamed));

    /*  Calls inside the lexical block belong to ordcall, and the
        nested caller does not leak into its caller.  */
    TASK_CHECK(dwarf_callsite_snapshot_function_call_count(snap,&ord,
        &count,&error) == DW_DLV_OK);
    TASK_CHECK(count == 3);
    TASK_CHECK(dwarf_callsite_snapshot_function_call_count(snap,
        &chain,&count,&error) == DW_DLV_OK);
    TASK_CHECK(count == 2);
    TASK_CHECK(dwarf_callsite_snapshot_function_call_count(snap,&fb,
        &count,&error) == DW_DLV_OK);
    TASK_CHECK(count == 1);
    TASK_CHECK(dwarf_callsite_snapshot_function_call_count(snap,
        &gate,&count,&error) == DW_DLV_OK);
    TASK_CHECK(count == 2);
    TASK_CHECK(dwarf_callsite_snapshot_function_call_count(snap,
        &unnamed,&count,&error) == DW_DLV_OK);
    TASK_CHECK(count == 2);
    /*  The tail flag and return PC are reported per call.  */
    {
        Dwarf_Off off = 0;
        Dwarf_Addr pc = 0;
        Dwarf_Bool has_pc = 0;
        Dwarf_Bool is_tail = 0;
        Dwarf_Unsigned i = 0;
        unsigned ordinary = 0;
        unsigned tail = 0;

        TASK_CHECK(dwarf_callsite_snapshot_function_call_count(snap,
            &ord,&count,&error) == DW_DLV_OK);
        for (i = 0; i < count; ++i) {
            TASK_CHECK(dwarf_callsite_snapshot_function_call(snap,
                &ord,i,&off,&pc,&has_pc,&is_tail,&error) ==
                DW_DLV_OK);
            TASK_CHECK(has_pc);
            TASK_CHECK(off != 0);
            TASK_CHECK(pc == 0x100 || pc == 0x101);
            if (is_tail) {
                ++tail;
            } else {
                ++ordinary;
            }
        }
        TASK_CHECK(ordinary == 3);
        TASK_CHECK(tail == 0);
    }
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

static int
case_identity_origin_resolution(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord;
    Dwarf_Callsite_Die_Identity chain;
    Dwarf_Callsite_Die_Identity clone;
    Dwarf_Callsite_Die_Identity fb;
    Dwarf_Callsite_Die_Identity fc;
    Dwarf_Callsite_Die_Identity fd;
    Dwarf_Callsite_Die_Identity fbc;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord));
    TASK_CHECK(find_fn_nth(snap,"chain",0,&chain));
    TASK_CHECK(find_fn_nth(snap,"chain",1,&clone));
    TASK_CHECK(find_fn_nth(snap,"fb",0,&fb));
    TASK_CHECK(find_fn_nth(snap,"fc",0,&fc));
    TASK_CHECK(find_fn_nth(snap,"fd",0,&fd));
    TASK_CHECK(find_fn_nth(snap,"fbc",0,&fbc));

    /*  ref4 within one CU.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Call call;
        Dwarf_Callsite_Chain ch;
        Dwarf_Callsite_Edge edge;

        TASK_CHECK(q != 0);
        memset(&call,0,sizeof(call));
        TASK_CHECK(dwarf_callsite_query_call(q,0,&call,&error) ==
            DW_DLV_OK);
        TASK_CHECK(call.cscall_origin_known);
        TASK_CHECK(same_identity(&call.cscall_origin,&chain));
        TASK_CHECK(call.cscall_chain_count == 3);
        memset(&ch,0,sizeof(ch));
        TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch,&error) ==
            DW_DLV_OK);
        TASK_CHECK(ch.csc_edge_count == 3);
        memset(&edge,0,sizeof(edge));
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,0,&edge,&error) ==
            DW_DLV_OK);
        TASK_CHECK(edge.cse_origin_known);
        TASK_CHECK(same_identity(&edge.cse_origin,&chain));
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,1,&edge,&error) ==
            DW_DLV_OK);
        TASK_CHECK(same_identity(&edge.cse_origin,&fb));
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,2,&edge,&error) ==
            DW_DLV_OK);
        TASK_CHECK(same_identity(&edge.cse_origin,&fc));
        TASK_CHECK(edge.cse_origin_attr_offset != 0);
        dwarf_callsite_query_destroy(q);
    }
    /*  ref_addr across CUs: the CU1 clone calls CU0 fb.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&clone,0x609);
        Dwarf_Callsite_Edge edge;

        TASK_CHECK(q != 0);
        memset(&edge,0,sizeof(edge));
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,0,&edge,&error) ==
            DW_DLV_OK);
        TASK_CHECK(edge.cse_origin_known);
        TASK_CHECK(same_identity(&edge.cse_origin,&fb));
        TASK_CHECK(!same_identity(&edge.cse_origin,&fbc));
        dwarf_callsite_query_destroy(q);
    }
    /*  Two calls share return PC 0x101 in ordcall.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x101);
        Dwarf_Unsigned calls = 0;
        Dwarf_Callsite_Call c0;
        Dwarf_Callsite_Call c1;

        TASK_CHECK(q != 0);
        TASK_CHECK(dwarf_callsite_query_call_count(q,&calls,&error) ==
            DW_DLV_OK);
        TASK_CHECK(calls == 2);
        memset(&c0,0,sizeof(c0));
        memset(&c1,0,sizeof(c1));
        TASK_CHECK(dwarf_callsite_query_call(q,0,&c0,&error) ==
            DW_DLV_OK);
        TASK_CHECK(dwarf_callsite_query_call(q,1,&c1,&error) ==
            DW_DLV_OK);
        TASK_CHECK(c0.cscall_call_site_offset !=
            c1.cscall_call_site_offset);
        TASK_CHECK(same_identity(&c0.cscall_origin,&chain));
        TASK_CHECK(same_identity(&c1.cscall_origin,&fd));
        dwarf_callsite_query_destroy(q);
    }
    /*  A return PC with no call site is reported as no entry.  */
    {
        Dwarf_Error qerror = 0;
        Dwarf_Callsite_Query q = 0;

        TASK_CHECK(dwarf_callsite_query(snap,&ord,0x999,&q,
            &qerror) == DW_DLV_NO_ENTRY);
        TASK_CHECK(q == 0);
        if (qerror) {
            dwarf_dealloc_error(0,qerror);
        }
    }
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

/*  ==================================================================
    Group substitution
    ================================================================== */

static int
case_substitution_forwarded_value(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord;
    Dwarf_Callsite_Die_Identity fb;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord));
    TASK_CHECK(find_fn_nth(snap,"fb",0,&fb));

    /*  The ordinary call sets r5 = 7.  The first tail edge passes
        r6 = entry(r5) + 3, so the terminal r6 is 10.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Param param;
        Dwarf_Unsigned plen = 0;
        const unsigned char *pbytes = 0;

        TASK_CHECK(q != 0);
        memset(&param,0,sizeof(param));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,1,0,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_register_known);
        TASK_CHECK(param.csp_register == 6);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 10);
        TASK_CHECK(param.csp_unknown_reason == DW_CS_UNKNOWN_NONE);
        TASK_CHECK(param.csp_expression_length ==
            sizeof(expr_entry_r5_plus3));
        TASK_CHECK(dwarf_callsite_query_param_expression(q,0,0,1,0,
            &pbytes,&plen,&error) == DW_DLV_OK);
        TASK_CHECK(plen == sizeof(expr_entry_r5_plus3));
        TASK_CHECK(bytes_equal(pbytes,expr_entry_r5_plus3,plen));
        TASK_CHECK(param.csp_expression_offset != 0);
        TASK_CHECK(param.csp_call_value_attr_offset != 0);
        /*  The other branch forwards entry(r5) + 100.  */
        TASK_CHECK(dwarf_callsite_query_param(q,0,1,1,0,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 107);
        TASK_CHECK(dwarf_callsite_query_param(q,0,1,1,1,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == -3);   /* consts -5 + 2 */
        TASK_CHECK(dwarf_callsite_query_param(q,0,1,1,2,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 6);    /* entry(r5=7) - 1 */
        dwarf_callsite_query_destroy(q);
    }
    /*  An unknown entry value keeps the source symbol, not zero. */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Param param;

        TASK_CHECK(q != 0);
        memset(&param,0,sizeof(param));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,2,0,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_register == 7);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 11);   /* entry(r6=10) + 1 */
        dwarf_callsite_query_destroy(q);
    }
    /*  An entry register that the previous edge never declared keeps
        the source symbol and its addend rather than inventing 0.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Param param;

        TASK_CHECK(q != 0);
        memset(&param,0,sizeof(param));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,2,1,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_register == 5);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_SYMBOLIC);
        TASK_CHECK(same_identity(&param.csp_source_function,&fb));
        TASK_CHECK(param.csp_source_register == 12);
        TASK_CHECK(param.csp_source_addend == 2);
        TASK_CHECK(param.csp_unknown_reason == DW_CS_UNKNOWN_NONE);
        dwarf_callsite_query_destroy(q);
    }
    /*  The ordinary call's own argument is known and its source is
        the forwarding function, not a later function.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Param param;

        TASK_CHECK(q != 0);
        memset(&param,0,sizeof(param));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,0,0,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_register == 5);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 7);
        dwarf_callsite_query_destroy(q);
    }
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

static int
case_substitution_unknown_isolation(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord));

    /*  An overflowed parameter must not disturb the known
        parameter beside it and must not stop the chain.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Param known;
        Dwarf_Callsite_Param over;
        Dwarf_Callsite_Chain ch;
        Dwarf_Unsigned plen = 0;
        const unsigned char *pbytes = 0;

        TASK_CHECK(q != 0);
        memset(&known,0,sizeof(known));
        memset(&over,0,sizeof(over));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,1,0,&known,
            &error) == DW_DLV_OK);
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,1,1,&over,
            &error) == DW_DLV_OK);
        TASK_CHECK(known.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(known.csp_value == 10);
        TASK_CHECK(over.csp_register == 8);
        TASK_CHECK(over.csp_value_kind == DW_CS_VALUE_UNKNOWN);
        TASK_CHECK(over.csp_unknown_reason == DW_CS_UNKNOWN_OVERFLOW);
        TASK_CHECK(over.csp_expression_length ==
            sizeof(expr_overflow));
        TASK_CHECK(dwarf_callsite_query_param_expression(q,0,0,1,1,
            &pbytes,&plen,&error) == DW_DLV_OK);
        TASK_CHECK(plen == sizeof(expr_overflow));
        TASK_CHECK(bytes_equal(pbytes,expr_overflow,plen));
        TASK_CHECK(over.csp_expression_offset != 0);
        memset(&ch,0,sizeof(ch));
        TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch,&error) ==
            DW_DLV_OK);
        TASK_CHECK(ch.csc_kind == DW_CS_CHAIN_COMPLETE);
        dwarf_callsite_query_destroy(q);
    }
    /*  Unsupported expression and missing value keep their own
        reasons and bytes on the same edge.  */
    {
        Dwarf_Callsite_Query q = query_fn(snap,&ord,0x100);
        Dwarf_Callsite_Param param;
        Dwarf_Unsigned plen = 0;
        const unsigned char *pbytes = 0;

        TASK_CHECK(q != 0);
        memset(&param,0,sizeof(param));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,2,2,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_UNKNOWN);
        TASK_CHECK(param.csp_unknown_reason ==
            DW_CS_UNKNOWN_UNSUPPORTED_EXPRESSION);
        TASK_CHECK(dwarf_callsite_query_param_expression(q,0,0,2,2,
            &pbytes,&plen,&error) == DW_DLV_OK);
        TASK_CHECK(plen == sizeof(expr_entry_r6_mul100));
        TASK_CHECK(bytes_equal(pbytes,expr_entry_r6_mul100,plen));
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,2,3,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_UNKNOWN);
        TASK_CHECK(param.csp_unknown_reason ==
            DW_CS_UNKNOWN_MISSING_VALUE);
        TASK_CHECK(param.csp_expression_length == 0);
        TASK_CHECK(dwarf_callsite_query_param_expression(q,0,0,2,3,
            &pbytes,&plen,&error) == DW_DLV_OK);
        TASK_CHECK(plen == 0);
        TASK_CHECK(pbytes == 0);
        /*  The known parameter on the same edge is unaffected. */
        TASK_CHECK(dwarf_callsite_query_param(q,0,0,2,0,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 11);
        dwarf_callsite_query_destroy(q);
    }
    /*  Indirect targets leave the origin unknown with a reason.  */
    {
        Dwarf_Callsite_Die_Identity unnamed;
        Dwarf_Callsite_Query q = 0;
        Dwarf_Callsite_Call call;
        Dwarf_Callsite_Chain ch;
        Dwarf_Callsite_Edge edge;

        TASK_CHECK(find_unnamed(snap,&unnamed));
        q = query_fn(snap,&unnamed,0x700);
        TASK_CHECK(q != 0);
        memset(&call,0,sizeof(call));
        TASK_CHECK(dwarf_callsite_query_call(q,0,&call,&error) ==
            DW_DLV_OK);
        TASK_CHECK(!call.cscall_origin_known);
        TASK_CHECK(call.cscall_origin_unknown_reason ==
            DW_CS_UNKNOWN_INDIRECT_TARGET);
        memset(&ch,0,sizeof(ch));
        TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch,&error) ==
            DW_DLV_OK);
        TASK_CHECK(ch.csc_kind == DW_CS_CHAIN_INCOMPLETE);
        memset(&edge,0,sizeof(edge));
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,0,&edge,&error) ==
            DW_DLV_OK);
        TASK_CHECK(!edge.cse_origin_known);
        TASK_CHECK(edge.cse_origin_unknown_reason ==
            DW_CS_UNKNOWN_INDIRECT_TARGET);
        TASK_CHECK(edge.cse_origin_attr_offset != 0);
        dwarf_callsite_query_destroy(q);
    }
    /*  DW_OP_regx names a register beyond DW_OP_reg31 and is
        accepted as a location.  The query at 0x101 has two calls;
        the second one targets fd and carries the regx parameter. */
    {
        Dwarf_Callsite_Die_Identity ord2;
        Dwarf_Callsite_Query q = 0;
        Dwarf_Callsite_Param param;
        Dwarf_Unsigned calls = 0;

        TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord2));
        q = query_fn(snap,&ord2,0x101);
        TASK_CHECK(q != 0);
        TASK_CHECK(dwarf_callsite_query_call_count(q,&calls,&error) ==
            DW_DLV_OK);
        TASK_CHECK(calls == 2);
        memset(&param,0,sizeof(param));
        TASK_CHECK(dwarf_callsite_query_param(q,1,0,0,0,&param,
            &error) == DW_DLV_OK);
        TASK_CHECK(param.csp_register_known);
        TASK_CHECK(param.csp_register == 40);
        TASK_CHECK(param.csp_value_kind == DW_CS_VALUE_KNOWN);
        TASK_CHECK(param.csp_value == 5);
        dwarf_callsite_query_destroy(q);
    }
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

/*  ==================================================================
    Group termination
    ================================================================== */

static int
case_termination_diamond_and_complete(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord;
    Dwarf_Callsite_Die_Identity chain;
    Dwarf_Callsite_Die_Identity fd;
    Dwarf_Callsite_Query q = 0;
    Dwarf_Callsite_Call call;
    Dwarf_Callsite_Chain ch0;
    Dwarf_Callsite_Chain ch1;
    Dwarf_Callsite_Chain ch2;
    Dwarf_Callsite_Edge edge;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord));
    TASK_CHECK(find_fn_nth(snap,"chain",0,&chain));
    TASK_CHECK(find_fn_nth(snap,"fd",0,&fd));
    q = query_fn(snap,&ord,0x100);
    TASK_CHECK(q != 0);
    memset(&call,0,sizeof(call));
    TASK_CHECK(dwarf_callsite_query_call(q,0,&call,&error) ==
        DW_DLV_OK);
    /*  Two complete branches reach fc.  "chain" declares no
        completeness, so one incomplete candidate is kept too. */
    TASK_CHECK(call.cscall_chain_count == 3);
    memset(&ch0,0,sizeof(ch0));
    memset(&ch1,0,sizeof(ch1));
    memset(&ch2,0,sizeof(ch2));
    TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch0,&error) ==
        DW_DLV_OK);
    TASK_CHECK(dwarf_callsite_query_chain(q,0,1,&ch1,&error) ==
        DW_DLV_OK);
    TASK_CHECK(dwarf_callsite_query_chain(q,0,2,&ch2,&error) ==
        DW_DLV_OK);
    TASK_CHECK(ch0.csc_kind == DW_CS_CHAIN_COMPLETE);
    TASK_CHECK(ch1.csc_kind == DW_CS_CHAIN_COMPLETE);
    TASK_CHECK(ch2.csc_kind == DW_CS_CHAIN_INCOMPLETE);
    TASK_CHECK(ch0.csc_edge_count == 3);
    TASK_CHECK(ch1.csc_edge_count == 3);
    TASK_CHECK(ch2.csc_edge_count == 1);
    /*  The two diamond branches are not globally deduplicated. */
    memset(&edge,0,sizeof(edge));
    TASK_CHECK(dwarf_callsite_query_edge(q,0,0,1,&edge,&error) ==
        DW_DLV_OK);
    TASK_CHECK(edge.cse_call_return_pc == 0x200);
    TASK_CHECK(edge.cse_is_tail_call);
    TASK_CHECK(dwarf_callsite_query_edge(q,0,1,1,&edge,&error) ==
        DW_DLV_OK);
    TASK_CHECK(edge.cse_call_return_pc == 0x201);
    TASK_CHECK(edge.cse_is_tail_call);
    TASK_CHECK(same_identity(&edge.cse_origin,&fd));
    /*  Each candidate keeps its own ordered call site offsets. */
    {
        Dwarf_Off site0 = 0;
        Dwarf_Off site1 = 0;

        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,0,&edge,
            &error) == DW_DLV_OK);
        site0 = edge.cse_call_site_offset;
        TASK_CHECK(dwarf_callsite_query_edge(q,0,1,0,&edge,
            &error) == DW_DLV_OK);
        site1 = edge.cse_call_site_offset;
        TASK_CHECK(site0 == site1);
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,1,&edge,
            &error) == DW_DLV_OK);
        TASK_CHECK(edge.cse_call_site_offset > site0);
        TASK_CHECK(dwarf_callsite_query_edge(q,0,0,2,&edge,
            &error) == DW_DLV_OK);
        TASK_CHECK(edge.cse_call_site_offset >
            site0);
    }
    dwarf_callsite_query_destroy(q);
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

static int
case_termination_incomplete_kept(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity gate;
    Dwarf_Callsite_Query q = 0;
    Dwarf_Callsite_Chain ch0;
    Dwarf_Callsite_Chain ch1;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"gate",0,&gate));
    /*  gate calls fe.  fe declares no completeness but does have a
        known tail edge, so an incomplete candidate is kept in
        addition to the complete path through fb and fc.  */
    q = query_fn(snap,&gate,0x520);
    TASK_CHECK(q != 0);
    memset(&ch0,0,sizeof(ch0));
    memset(&ch1,0,sizeof(ch1));
    TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch0,&error) ==
        DW_DLV_OK);
    TASK_CHECK(dwarf_callsite_query_chain(q,0,1,&ch1,&error) ==
        DW_DLV_OK);
    TASK_CHECK(ch0.csc_kind == DW_CS_CHAIN_COMPLETE);
    TASK_CHECK(ch0.csc_edge_count == 3);
    TASK_CHECK(ch1.csc_kind == DW_CS_CHAIN_INCOMPLETE);
    /*  The incomplete candidate ends at fe, before its known tail
        edge, so it holds only the gate -> fe edge.  */
    TASK_CHECK(ch1.csc_edge_count == 1);
    dwarf_callsite_query_destroy(q);
    /*  gate calls loose, which declares no completeness and has no
        tail exits: the only candidate is incomplete.  */
    q = query_fn(snap,&gate,0x521);
    TASK_CHECK(q != 0);
    memset(&ch0,0,sizeof(ch0));
    TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch0,&error) ==
        DW_DLV_OK);
    TASK_CHECK(ch0.csc_kind == DW_CS_CHAIN_INCOMPLETE);
    TASK_CHECK(ch0.csc_edge_count == 1);
    dwarf_callsite_query_destroy(q);
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

static int
case_termination_cycle(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity cyca;
    Dwarf_Callsite_Die_Identity cycb;
    Dwarf_Callsite_Query q = 0;
    Dwarf_Callsite_Chain ch;
    Dwarf_Callsite_Edge edge;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"cyca",0,&cyca));
    TASK_CHECK(find_fn_nth(snap,"cycb",0,&cycb));
    q = query_fn(snap,&cyca,0x600);
    TASK_CHECK(q != 0);
    memset(&ch,0,sizeof(ch));
    TASK_CHECK(dwarf_callsite_query_chain(q,0,0,&ch,&error) ==
        DW_DLV_OK);
    TASK_CHECK(ch.csc_kind == DW_CS_CHAIN_CYCLE);
    TASK_CHECK(ch.csc_edge_count == 2);
    memset(&edge,0,sizeof(edge));
    TASK_CHECK(dwarf_callsite_query_edge(q,0,0,0,&edge,&error) ==
        DW_DLV_OK);
    TASK_CHECK(same_identity(&edge.cse_origin,&cycb));
    TASK_CHECK(dwarf_callsite_query_edge(q,0,0,1,&edge,&error) ==
        DW_DLV_OK);
    TASK_CHECK(same_identity(&edge.cse_origin,&cyca));
    TASK_CHECK(edge.cse_is_tail_call);
    /*  Only this candidate stops: the query has exactly one
        chain and no false complete ending.  */
    {
        Dwarf_Callsite_Call call;

        memset(&call,0,sizeof(call));
        TASK_CHECK(dwarf_callsite_query_call(q,0,&call,&error) ==
            DW_DLV_OK);
        TASK_CHECK(call.cscall_chain_count == 1);
    }
    dwarf_callsite_query_destroy(q);
    dwarf_callsite_snapshot_destroy(snap);
    dwarf_finish(dbg);
    return 0;
}

/*  Reverse order repeated queries after dwarf_finish: results stay
    isolated, printable and deallocatable.  */
static int
case_termination_reverse_and_lifetime(void)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Callsite_Snapshot snap = 0;
    Dwarf_Error error = 0;
    Dwarf_Callsite_Die_Identity ord;
    Dwarf_Callsite_Die_Identity gate;
    Dwarf_Callsite_Query q_first = 0;
    Dwarf_Callsite_Query q_second = 0;
    Dwarf_Callsite_Param p_first;
    Dwarf_Callsite_Param p_second;
    Dwarf_Unsigned n_first = 0;
    Dwarf_Unsigned n_second = 0;

    TASK_CHECK(open_fixture(&dbg));
    TASK_CHECK(dwarf_callsite_snapshot_create(dbg,&snap,&error) ==
        DW_DLV_OK);
    TASK_CHECK(find_fn_nth(snap,"ordcall",0,&ord));
    TASK_CHECK(find_fn_nth(snap,"gate",0,&gate));
    q_first = query_fn(snap,&ord,0x100);
    q_second = query_fn(snap,&gate,0x520);
    TASK_CHECK(q_first != 0);
    TASK_CHECK(q_second != 0);
    TASK_CHECK(dwarf_callsite_query_call_count(q_first,&n_first,
        &error) == DW_DLV_OK);
    TASK_CHECK(dwarf_callsite_query_call_count(q_second,&n_second,
        &error) == DW_DLV_OK);
    TASK_CHECK(n_first == 1);
    TASK_CHECK(n_second == 1);
    /*  Destroy the snapshot and finish before using the results. */
    dwarf_callsite_snapshot_destroy(snap);
    snap = 0;
    dwarf_finish(dbg);
    dbg = 0;
    memset(&p_first,0,sizeof(p_first));
    memset(&p_second,0,sizeof(p_second));
    TASK_CHECK(dwarf_callsite_query_param(q_first,0,0,1,0,&p_first,
        &error) == DW_DLV_OK);
    TASK_CHECK(dwarf_callsite_query_param(q_second,0,0,0,0,&p_second,
        &error) == DW_DLV_OK);
    TASK_CHECK(p_first.csp_value_kind == DW_CS_VALUE_KNOWN);
    TASK_CHECK(p_first.csp_value == 10);
    TASK_CHECK(p_second.csp_value_kind == DW_CS_VALUE_KNOWN);
    TASK_CHECK(p_second.csp_value == 5);
    /*  Repeat in the opposite order: the same isolated answers. */
    {
        Dwarf_Callsite_Param again_first;
        Dwarf_Callsite_Param again_second;

        memset(&again_first,0,sizeof(again_first));
        memset(&again_second,0,sizeof(again_second));
        TASK_CHECK(dwarf_callsite_query_param(q_second,0,0,0,0,
            &again_second,&error) == DW_DLV_OK);
        TASK_CHECK(dwarf_callsite_query_param(q_first,0,0,1,0,
            &again_first,&error) == DW_DLV_OK);
        TASK_CHECK(again_first.csp_value == p_first.csp_value);
        TASK_CHECK(again_second.csp_value == p_second.csp_value);
        TASK_CHECK(same_identity(&again_first.csp_source_function,
            &p_first.csp_source_function));
        TASK_CHECK(again_second.csp_register ==
            p_second.csp_register);
    }
    /*  Printing works with no Dwarf_Debug alive.  */
    {
        g_print_lines = 0;
        TASK_CHECK(dwarf_callsite_query_print(q_first,count_line,0,
            &error) == DW_DLV_OK);
        TASK_CHECK(g_print_lines > 0);
        g_print_lines = 0;
        TASK_CHECK(dwarf_callsite_query_print(q_second,count_line,0,
            &error) == DW_DLV_OK);
        TASK_CHECK(g_print_lines > 0);
    }
    dwarf_callsite_query_destroy(q_first);
    dwarf_callsite_query_destroy(q_second);
    /*  Destroying NULL and passing NULL are harmless.  */
    dwarf_callsite_query_destroy(0);
    dwarf_callsite_snapshot_destroy(0);
    return 0;
}

/*  ---- case table ---- */

const struct task_case task_cases[] = {
    {"identity","names_and_offsets",case_identity_names_and_offsets},
    {"identity","call_ownership",case_identity_call_ownership},
    {"identity","origin_resolution",case_identity_origin_resolution},
    {"substitution","forwarded_value",
        case_substitution_forwarded_value},
    {"substitution","unknown_isolation",
        case_substitution_unknown_isolation},
    {"termination","diamond_and_complete",
        case_termination_diamond_and_complete},
    {"termination","incomplete_kept",
        case_termination_incomplete_kept},
    {"termination","cycle",case_termination_cycle},
    {"termination","reverse_and_lifetime",
        case_termination_reverse_and_lifetime}
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
