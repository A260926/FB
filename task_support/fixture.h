#ifndef TASK_MACHO_FIXTURE_H
#define TASK_MACHO_FIXTURE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Legal synthetic Mach-O, never executable: no entry point, inert text bytes. */
static void fixture_u32(unsigned char *b, size_t o, uint32_t x) {
    for (unsigned i=0;i<4;i++) b[o+i]=(unsigned char)(x>>(8*i));
}
static void fixture_u64(unsigned char *b, size_t o, uint64_t x) {
    for (unsigned i=0;i<8;i++) b[o+i]=(unsigned char)(x>>(8*i));
}
static int fixture_write(const char *path, size_t prefix) {
    unsigned char b[2048]={0};
    fixture_u32(b,0,0xfeedfacf);fixture_u32(b,4,0x01000007);fixture_u32(b,8,3);
    fixture_u32(b,12,6);fixture_u32(b,16,5);fixture_u32(b,20,408);fixture_u32(b,24,0x80);
    size_t s=32;
    fixture_u32(b,s,0x19);fixture_u32(b,s+4,232);memcpy(b+s+8,"__DATA",6);
    fixture_u64(b,s+24,0x100000000ULL);fixture_u64(b,s+32,2048);
    fixture_u64(b,s+40,0);fixture_u64(b,s+48,sizeof b);
    fixture_u32(b,s+56,7);fixture_u32(b,s+60,5);fixture_u32(b,s+64,2);
    size_t t=s+72;
    memcpy(b+t,"__text",6);memcpy(b+t+16,"__DATA",6);
    fixture_u64(b,t+32,0x100000400ULL);fixture_u64(b,t+40,16);
    fixture_u32(b,t+48,1024);fixture_u32(b,t+52,2);fixture_u32(b,t+64,0x80000400);
    t+=80;memcpy(b+t,"__nl_symbol_ptr",15);memcpy(b+t+16,"__DATA",6);
    fixture_u64(b,t+32,0x100000420ULL);fixture_u64(b,t+40,16);
    fixture_u32(b,t+48,1056);fixture_u32(b,t+52,3);fixture_u32(b,t+64,6);
    s+=232;fixture_u32(b,s,2);fixture_u32(b,s+4,24);
    fixture_u32(b,s+8,1280);fixture_u32(b,s+12,1);
    fixture_u32(b,s+16,1408);fixture_u32(b,s+20,64);
    s+=24;fixture_u32(b,s,11);fixture_u32(b,s+4,80);
    fixture_u32(b,s+24,0);fixture_u32(b,s+28,1);
    fixture_u32(b,s+56,1536);fixture_u32(b,s+60,2);
    s+=80;fixture_u32(b,s,12);fixture_u32(b,s+4,40);
    fixture_u32(b,s+8,24);memcpy(b+s+24,"libAlpha.dylib",15);
    s+=40;fixture_u32(b,s,12);fixture_u32(b,s+4,32);
    fixture_u32(b,s+8,24);memcpy(b+s+24,"libB",5);
    fixture_u32(b,1280,1);b[1284]=1;b[1287]=1;
    memcpy(b+1409,"_alpha",7);fixture_u32(b,1536,0);fixture_u32(b,1540,0x80000000);
    FILE *f=fopen(path,"wb");if(!f)return 1;
    for(size_t i=0;i<prefix;i++)if(fputc(0xa5,f)==EOF){fclose(f);return 1;}
    int ok=fwrite(b,1,sizeof b,f)==sizeof b;return fclose(f)!=0 || !ok;
}

/* ------------------------------------------------------------------ */
/* Configurable thin Mach-O builder for the macho_references tests.    */
/* All images are inert synthetic byte layouts, never executable.      */
/* ------------------------------------------------------------------ */

#define MFIX_MAX_SEC 12
#define MFIX_MAX_NL 64
#define MFIX_MAX_DYLIB 12

#define MFIX_LC_SEGMENT        0x1u
#define MFIX_LC_SYMTAB         0x2u
#define MFIX_LC_DYSYMTAB       0xbu
#define MFIX_LC_LOAD_DYLIB     0xcu
#define MFIX_LC_ID_DYLIB       0xdu
#define MFIX_LC_LOAD_WEAK      0x80000018u
#define MFIX_LC_REEXPORT       0x8000001fu
#define MFIX_LC_UPWARD         0x80000023u

#define MFIX_INDIRECT_LOCAL    0x80000000u
#define MFIX_INDIRECT_ABS      0x40000000u

typedef struct {
    const char *sect;      /* section name, default "__nl_symbol_ptr" */
    const char *seg;       /* segment name, default "__DATA" */
    uint32_t type;         /* section low type 6/7/8 */
    uint32_t raw_flags;    /* 0 -> type only; otherwise raw section flags */
    uint64_t addr;         /* 0 -> auto */
    uint64_t size;         /* explicit section byte size */
    uint32_t stub_width;   /* reserved2 for S_SYMBOL_STUBS */
    uint32_t reserved1;    /* explicit indirect start when reserved1_set */
    int reserved1_set;     /* 0 -> packed automatic indirect start */
    uint32_t offset;       /* 0 -> automatically placed data offset */
    const uint32_t *ind;   /* raw indirect values for this section */
    uint32_t ind_n;
} mfix_section;

typedef struct {
    const char *name;      /* appended to the string table when !strx_set */
    uint32_t strx;         /* explicit string offset when strx_set */
    int strx_set;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} mfix_nlist;

typedef struct {
    uint32_t cmd;          /* MFIX_LC_LOAD_DYLIB / _ID / _WEAK ... */
    const char *path;
} mfix_dylib;

/* override bitmask for mfix_config.ovmask */
#define MFIX_OV_NIND          0x01u
#define MFIX_OV_INDIRECTOFF   0x02u
#define MFIX_OV_SYMOFF        0x04u
#define MFIX_OV_NSYMS         0x08u
#define MFIX_OV_STROFF        0x10u
#define MFIX_OV_STRSIZE       0x20u

typedef struct {
    int bits;              /* 32 or 64 (default 64) */
    int big_endian;
    int reversed_cmds;     /* reverse chunk order (dylibs..segments) */
    int far_tables;        /* put a wide gap before the indirect table */
    const mfix_section *secs;
    size_t nsec;
    const mfix_nlist *nls;
    size_t nnl;
    const mfix_dylib *dylibs;
    size_t ndylib;
    int no_dysymtab;
    int no_symtab;
    unsigned ovmask;       /* MFIX_OV_*: which ov_* fields replace computed */
    uint32_t ov_nind;
    uint32_t ov_indirectoff;
    uint32_t ov_symoff;
    uint32_t ov_nsyms;
    uint32_t ov_stroff;
    uint32_t ov_strsize;
    size_t truncate_to;    /* 0 keeps all bytes; otherwise truncate image */
} mfix_config;

typedef struct {
    unsigned char *d;
    size_t n;
    uint32_t indirectoff;
    uint32_t nind;
    uint32_t symoff, nsyms, stroff, strsize;
    uint32_t sec_reserved1[MFIX_MAX_SEC];
    uint32_t sec_offset[MFIX_MAX_SEC];
} mfix_image;

static void mfix__put32(unsigned char *b, size_t o, uint32_t x, int be) {
    if (be) { b[o]=(unsigned char)(x>>24); b[o+1]=(unsigned char)(x>>16);
              b[o+2]=(unsigned char)(x>>8);  b[o+3]=(unsigned char)x; }
    else    { b[o]=(unsigned char)x; b[o+1]=(unsigned char)(x>>8);
              b[o+2]=(unsigned char)(x>>16); b[o+3]=(unsigned char)(x>>24); }
}
static void mfix__put64(unsigned char *b, size_t o, uint64_t x, int be) {
    if (be) { for (int i=0;i<8;i++) b[o+i]=(unsigned char)(x>>(56-8*i)); }
    else    { for (int i=0;i<8;i++) b[o+i]=(unsigned char)(x>>(8*i)); }
}
static void mfix__put16(unsigned char *b, size_t o, uint16_t x, int be) {
    if (be) { b[o]=(unsigned char)(x>>8); b[o+1]=(unsigned char)x; }
    else    { b[o]=(unsigned char)x; b[o+1]=(unsigned char)(x>>8); }
}
static uint32_t mfix__align32(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

static void mfix_free(mfix_image *im) {
    if (im) { free(im->d); im->d = NULL; im->n = 0; }
}

static int mfix_build(const mfix_config *cfg, mfix_image *out) {
    int bits = cfg->bits == 32 ? 32 : 64;
    int be = cfg->big_endian ? 1 : 0;
    uint32_t hsize = bits == 64 ? 32u : 28u;
    uint32_t seg_hsize = bits == 64 ? 72u : 56u;
    uint32_t sect_size = bits == 64 ? 80u : 68u;
    uint32_t nlist_size = bits == 64 ? 16u : 12u;
    size_t i, k;
    uint32_t nseg_runs = 0, ic;
    uint32_t cmdsize_total = 0;
    uint32_t pos_seg = 0, pos_sym = 0, pos_dys = 0, pos_dylib = 0;
    uint32_t sym_cmdsize = cfg->no_symtab ? 0 : 24;
    uint32_t dys_cmdsize = cfg->no_dysymtab ? 0 : 80;
    uint32_t dylib_cmdsize[MFIX_MAX_DYLIB];
    uint32_t dylib_total = 0;
    uint32_t seg_total = 0;
    uint32_t cursor;
    uint32_t sec_off[MFIX_MAX_SEC];
    uint32_t sec_r1[MFIX_MAX_SEC];
    uint32_t indirect_extent = 0;
    uint32_t *ind_words = NULL;
    uint32_t symoff, stroff, indirectoff;
    uint32_t strsize = 1;
    unsigned char *strblob = NULL;
    uint32_t strx_of[MFIX_MAX_NL];
    size_t image_cap;

    memset(out, 0, sizeof(*out));

    if (cfg->nsec > MFIX_MAX_SEC || cfg->nnl > MFIX_MAX_NL ||
        cfg->ndylib > MFIX_MAX_DYLIB)
        return 1;

    /* count segment runs (new run whenever segment name changes) */
    for (i = 0; i < cfg->nsec; i++) {
        if (i == 0) nseg_runs++;
        else {
            const char *a = cfg->secs[i-1].seg ? cfg->secs[i-1].seg : "__DATA";
            const char *b = cfg->secs[i].seg ? cfg->secs[i].seg : "__DATA";
            if (strcmp(a, b) != 0) nseg_runs++;
        }
    }
    for (i = 0; i < cfg->nsec; ) {
        size_t j = i;
        const char *seg = cfg->secs[i].seg ? cfg->secs[i].seg : "__DATA";
        while (j < cfg->nsec) {
            const char *s2 = cfg->secs[j].seg ? cfg->secs[j].seg : "__DATA";
            if (strcmp(s2, seg) != 0) break;
            j++;
        }
        seg_total += seg_hsize + (uint32_t)(j - i) * sect_size;
        i = j;
    }

    for (i = 0; i < cfg->ndylib; i++) {
        size_t ln = strlen(cfg->dylibs[i].path ? cfg->dylibs[i].path : "") + 1;
        uint32_t cs = mfix__align32(24u + (uint32_t)ln, 8);
        dylib_cmdsize[i] = cs;
        dylib_total += cs;
    }

    cmdsize_total = seg_total + sym_cmdsize + dys_cmdsize + dylib_total;

    /* command chunk positions */
    if (!cfg->reversed_cmds) {
        pos_seg = hsize;
        pos_sym = pos_seg + seg_total;
        pos_dys = pos_sym + sym_cmdsize;
        pos_dylib = pos_dys + dys_cmdsize;
    } else {
        pos_dylib = hsize;
        pos_dys = pos_dylib + dylib_total;
        pos_sym = pos_dys + dys_cmdsize;
        pos_seg = pos_sym + sym_cmdsize;
    }
    (void)cmdsize_total;

    cursor = mfix__align32(hsize + cmdsize_total, 16);

    /* section data offsets: offset 0 means automatically placed */
    for (i = 0; i < cfg->nsec; i++) {
        if (cfg->secs[i].offset != 0) {
            sec_off[i] = cfg->secs[i].offset;
        } else {
            cursor = mfix__align32(cursor, 16);
            sec_off[i] = cursor;
            cursor += (uint32_t)cfg->secs[i].size;
        }
        if (sec_off[i] + (uint32_t)cfg->secs[i].size > cursor)
            cursor = sec_off[i] + (uint32_t)cfg->secs[i].size;
    }

    /* indirect index assignment.  Automatic reserved1 values pack the words
     * into the table; an explicit reserved1 (e.g. an intentionally bad one)
     * is only written into the section field and does not extend the table. */
    for (i = 0; i < cfg->nsec; i++) {
        if (cfg->secs[i].reserved1_set) {
            sec_r1[i] = cfg->secs[i].reserved1;
        } else {
            sec_r1[i] = indirect_extent;
            if ((uint64_t)indirect_extent + cfg->secs[i].ind_n > UINT32_MAX)
                return 1;
            indirect_extent += cfg->secs[i].ind_n;
        }
    }
    ind_words = (uint32_t*)calloc(indirect_extent ? indirect_extent : 1, sizeof(uint32_t));
    if (!ind_words) return 1;
    for (i = 0; i < cfg->nsec; i++) {
        if (cfg->secs[i].reserved1_set)
            continue; /* explicit start: do not place words at that index */
        for (k = 0; k < cfg->secs[i].ind_n; k++)
            ind_words[sec_r1[i] + k] = cfg->secs[i].ind[k];
    }

    if (cfg->far_tables) cursor += 4096;
    cursor = mfix__align32(cursor, 4);
    indirectoff = cursor;
    cursor += indirect_extent * 4;

    /* string table blob */
    strblob = (unsigned char*)calloc(1, 4096);
    if (!strblob) { free(ind_words); return 1; }
    strsize = 1; strblob[0] = 0;
    for (i = 0; i < cfg->nnl; i++) {
        if (cfg->nls[i].strx_set) {
            strx_of[i] = cfg->nls[i].strx;
        } else if (cfg->nls[i].name) {
            size_t ln = strlen(cfg->nls[i].name) + 1;
            strx_of[i] = strsize;
            memcpy(strblob + strsize, cfg->nls[i].name, ln);
            strsize += (uint32_t)ln;
        } else {
            strx_of[i] = 0;
        }
    }

    cursor = mfix__align32(cursor, 8);
    symoff = cursor;
    cursor += (uint32_t)cfg->nnl * nlist_size;
    stroff = cursor;
    cursor += strsize;

    image_cap = cursor + 64;
    out->d = (unsigned char*)calloc(1, image_cap);
    if (!out->d) { free(ind_words); free(strblob); return 1; }
    out->n = cursor;

    /* mach header */
    {
        uint32_t magic;
        if (bits == 64) magic = 0xfeedfacfu; else magic = 0xfeedfaceu;
        mfix__put32(out->d, 0, magic, be);
        mfix__put32(out->d, 4, bits == 64 ? 0x01000007u : 7u, be);
        mfix__put32(out->d, 8, 3, be);
        mfix__put32(out->d, 12, 6, be); /* MH_DYLIB synthetic */
        mfix__put32(out->d, 16, nseg_runs + (cfg->no_symtab?0:1) +
                              (cfg->no_dysymtab?0:1) + (uint32_t)cfg->ndylib, be);
        mfix__put32(out->d, 20, cmdsize_total, be);
        mfix__put32(out->d, 24, 0x80, be);
    }

    /* segment commands + sections */
    {
        uint32_t run_start = pos_seg;
        for (i = 0; i < cfg->nsec; ) {
            size_t j = i;
            const char *seg = cfg->secs[i].seg ? cfg->secs[i].seg : "__DATA";
            uint32_t nsect;
            size_t z;
            while (j < cfg->nsec) {
                const char *s2 = cfg->secs[j].seg ? cfg->secs[j].seg : "__DATA";
                if (strcmp(s2, seg) != 0) break;
                j++;
            }
            nsect = (uint32_t)(j - i);
            mfix__put32(out->d, run_start, bits==64?MFIX_LC_SEGMENT+0x18:MFIX_LC_SEGMENT, be);
            mfix__put32(out->d, run_start+4, seg_hsize + nsect*sect_size, be);
            memcpy(out->d + run_start + 8, seg, strnlen(seg, 16));
            if (bits == 64) {
                mfix__put64(out->d, run_start+24, 0x100000000ULL, be);
                mfix__put64(out->d, run_start+32, out->n, be);
                mfix__put64(out->d, run_start+40, 0, be);
                mfix__put64(out->d, run_start+48, out->n, be);
                mfix__put32(out->d, run_start+56, 7, be);
                mfix__put32(out->d, run_start+60, 5, be);
                mfix__put32(out->d, run_start+64, nsect, be);
                mfix__put32(out->d, run_start+68, 0, be);
            } else {
                mfix__put32(out->d, run_start+24, 0x10000000u, be);
                mfix__put32(out->d, run_start+28, (uint32_t)out->n, be);
                mfix__put32(out->d, run_start+32, 0, be);
                mfix__put32(out->d, run_start+36, (uint32_t)out->n, be);
                mfix__put32(out->d, run_start+40, 7, be);
                mfix__put32(out->d, run_start+44, 5, be);
                mfix__put32(out->d, run_start+48, nsect, be);
                mfix__put32(out->d, run_start+52, 0, be);
            }
            for (z = i; z < j; z++) {
                uint32_t so = run_start + seg_hsize + (uint32_t)(z - i) * sect_size;
                const char *sn = cfg->secs[z].sect ? cfg->secs[z].sect : "__nl_symbol_ptr";
                uint32_t flags = cfg->secs[z].raw_flags ?
                                 cfg->secs[z].raw_flags : cfg->secs[z].type;
                uint64_t addr = cfg->secs[z].addr ? cfg->secs[z].addr :
                                0x100001000ULL + sec_off[z];
                memcpy(out->d + so, sn, strnlen(sn, 16));
                memcpy(out->d + so + 16, seg, strnlen(seg, 16));
                if (bits == 64) {
                    mfix__put64(out->d, so+32, addr, be);
                    mfix__put64(out->d, so+40, cfg->secs[z].size, be);
                    mfix__put32(out->d, so+48, sec_off[z], be);
                    mfix__put32(out->d, so+52, 2, be);
                    mfix__put32(out->d, so+64, flags, be);
                    mfix__put32(out->d, so+68, sec_r1[z], be);
                    mfix__put32(out->d, so+72, cfg->secs[z].stub_width, be);
                } else {
                    mfix__put32(out->d, so+32, (uint32_t)addr, be);
                    mfix__put32(out->d, so+36, (uint32_t)cfg->secs[z].size, be);
                    mfix__put32(out->d, so+40, sec_off[z], be);
                    mfix__put32(out->d, so+44, 2, be);
                    mfix__put32(out->d, so+56, flags, be);
                    mfix__put32(out->d, so+60, sec_r1[z], be);
                    mfix__put32(out->d, so+64, cfg->secs[z].stub_width, be);
                }
            }
            run_start += seg_hsize + nsect * sect_size;
            i = j;
        }
    }

    /* symtab command */
    if (!cfg->no_symtab) {
        mfix__put32(out->d, pos_sym, MFIX_LC_SYMTAB, be);
        mfix__put32(out->d, pos_sym+4, 24, be);
        mfix__put32(out->d, pos_sym+8,  (cfg->ovmask & MFIX_OV_SYMOFF)  ? cfg->ov_symoff : symoff, be);
        mfix__put32(out->d, pos_sym+12, (cfg->ovmask & MFIX_OV_NSYMS)   ? cfg->ov_nsyms : (uint32_t)cfg->nnl, be);
        mfix__put32(out->d, pos_sym+16, (cfg->ovmask & MFIX_OV_STROFF)   ? cfg->ov_stroff : stroff, be);
        mfix__put32(out->d, pos_sym+20, (cfg->ovmask & MFIX_OV_STRSIZE)  ? cfg->ov_strsize : strsize, be);
    }

    /* dysymtab command: only indirectsymoff/nindirect are populated */
    if (!cfg->no_dysymtab) {
        mfix__put32(out->d, pos_dys, MFIX_LC_DYSYMTAB, be);
        mfix__put32(out->d, pos_dys+4, 80, be);
        mfix__put32(out->d, pos_dys+0x38,
                    (cfg->ovmask & MFIX_OV_INDIRECTOFF) ? cfg->ov_indirectoff : indirectoff, be);
        mfix__put32(out->d, pos_dys+0x3c,
                    (cfg->ovmask & MFIX_OV_NIND) ? cfg->ov_nind : indirect_extent, be);
    }

    /* dylib commands */
    for (i = 0; i < cfg->ndylib; i++) {
        uint32_t off = pos_dylib;
        const char *path = cfg->dylibs[i].path ? cfg->dylibs[i].path : "";
        mfix__put32(out->d, off, cfg->dylibs[i].cmd, be);
        mfix__put32(out->d, off+4, dylib_cmdsize[i], be);
        mfix__put32(out->d, off+8, 24, be); /* lc_str offset */
        memcpy(out->d + off + 24, path, strlen(path) + 1);
        pos_dylib += dylib_cmdsize[i];
    }

    /* section data: inert pattern bytes */
    for (i = 0; i < cfg->nsec; i++) {
        uint64_t z;
        for (z = 0; z < cfg->secs[i].size && sec_off[i] + z < out->n; z++)
            out->d[sec_off[i] + z] = (unsigned char)(0x40 + (z & 0xf));
    }

    /* indirect table */
    for (ic = 0; ic < indirect_extent; ic++)
        mfix__put32(out->d, indirectoff + ic*4, ind_words[ic], be);

    /* nlist table */
    for (i = 0; i < cfg->nnl; i++) {
        uint32_t no = symoff + (uint32_t)i * nlist_size;
        mfix__put32(out->d, no, strx_of[i], be);
        out->d[no+4] = cfg->nls[i].n_type;
        out->d[no+5] = cfg->nls[i].n_sect;
        mfix__put16(out->d, no+6, cfg->nls[i].n_desc, be);
        if (bits == 64)
            mfix__put64(out->d, no+8, cfg->nls[i].n_value, be);
        else
            mfix__put32(out->d, no+8, (uint32_t)cfg->nls[i].n_value, be);
    }

    /* string table */
    memcpy(out->d + stroff, strblob, strsize);

    for (i = 0; i < cfg->nsec && i < MFIX_MAX_SEC; i++) {
        out->sec_reserved1[i] = sec_r1[i];
        out->sec_offset[i] = sec_off[i];
    }
    out->indirectoff = indirectoff;
    out->nind = indirect_extent;
    out->symoff = symoff; out->nsyms = (uint32_t)cfg->nnl;
    out->stroff = stroff; out->strsize = strsize;

    if (cfg->truncate_to > 0 && cfg->truncate_to < out->n)
        out->n = cfg->truncate_to;

    free(ind_words);
    free(strblob);
    return 0;
}

static int mfix_write(const char *path, size_t prefix, const mfix_image *im) {
    FILE *f = fopen(path, "wb");
    size_t i;
    int ok;
    if (!f) return 1;
    for (i = 0; i < prefix; i++)
        if (fputc(0xa5, f) == EOF) { fclose(f); return 1; }
    ok = fwrite(im->d, 1, im->n, f) == im->n;
    return fclose(f) != 0 || !ok;
}
#endif
