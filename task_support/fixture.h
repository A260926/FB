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

/* ---- parameterized synthetic Mach-O builder: inert bytes, never executed ---- */

#define FIXTURE_LC_SEGMENT 0x1u
#define FIXTURE_LC_SYMTAB 0x2u
#define FIXTURE_LC_DYSYMTAB 0xbu
#define FIXTURE_LC_SEGMENT_64 0x19u

typedef struct FixtureSectionDef {
    const char* seg;      /* segment name (up to 16 chars) */
    const char* name;     /* section name (up to 16 chars) */
    uint64_t addr;        /* vm address of the section */
    uint64_t size;        /* section size in bytes */
    uint32_t offset;      /* image relative file offset of the content */
    uint32_t flags;       /* section flags: type | attributes */
    uint32_t reserved1;   /* indirect table start index for indirect sections */
    uint32_t reserved2;   /* stub size for S_SYMBOL_STUBS */
} FixtureSectionDef;

typedef struct FixtureSymbolDef {
    const char* name;     /* symbol name; strx is computed; NULL: use strx as-is */
    uint32_t strx;        /* explicit string table index, used when name == NULL */
    uint8_t type;         /* n_type */
    uint8_t sect;         /* n_sect */
    uint16_t desc;        /* n_desc (library ordinal in bits 8..15) */
    uint64_t value;       /* n_value */
} FixtureSymbolDef;

typedef struct FixtureDylibDef {
    uint32_t cmd;         /* LC_LOAD_DYLIB 0xc, LC_ID_DYLIB 0xd, LC_LOAD_WEAK_DYLIB 0x80000018,
                             LC_REEXPORT_DYLIB 0x8000001f, LC_LOAD_UPWARD_DYLIB 0x80000023,
                             LC_LAZY_LOAD_DYLIB 0x20 */
    const char* name;
} FixtureDylibDef;

typedef struct FixtureMachODef {
    uint8_t bitness;                 /* 32 | 64 */
    uint8_t big_endian;              /* 0 = little, 1 = big */
    uint32_t filetype;               /* MH_* */
    uint32_t flags;                  /* mach header flags */
    const FixtureSectionDef* sections; /* consecutive equal seg names form one segment command */
    uint32_t nsections;
    const FixtureSymbolDef* symbols;   /* NULL/0 => no LC_SYMTAB */
    uint32_t nsymbols;
    const uint32_t* indirect;          /* NULL/0 => no LC_DYSYMTAB */
    uint32_t nindirect;
    const FixtureDylibDef* dylibs;
    uint32_t ndylibs;
    uint32_t symoff;          /* 0 => auto layout after the commands */
    uint32_t stroff;          /* 0 => auto */
    uint32_t strsize;         /* 0 => auto (exact fit); may truncate names on purpose */
    uint32_t indirectsymoff;  /* 0 => auto */
    uint32_t filesize;        /* 0 => auto; may truncate tables on purpose */
} FixtureMachODef;

static void fixture_u16e(unsigned char *b, size_t o, uint16_t x, int be) {
    if (be) { b[o]=(unsigned char)(x>>8); b[o+1]=(unsigned char)x; }
    else { b[o]=(unsigned char)x; b[o+1]=(unsigned char)(x>>8); }
}
static void fixture_u32e(unsigned char *b, size_t o, uint32_t x, int be) {
    unsigned i;
    if (be) for (i=0;i<4;i++) b[o+i]=(unsigned char)(x>>(8*(3-i)));
    else for (i=0;i<4;i++) b[o+i]=(unsigned char)(x>>(8*i));
}
static void fixture_u64e(unsigned char *b, size_t o, uint64_t x, int be) {
    unsigned i;
    if (be) for (i=0;i<8;i++) b[o+i]=(unsigned char)(x>>(8*(7-i)));
    else for (i=0;i<8;i++) b[o+i]=(unsigned char)(x>>(8*i));
}
static uint32_t fixture_align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}
static void fixture_name16(unsigned char *b, size_t o, const char *s) {
    size_t i;
    for (i=0;i<16 && s[i];i++) b[o+i]=(unsigned char)s[i];
}

/* Write the image described by m after prefix filler bytes. Returns 0 on success. */
static int fixture_write_macho(const char *path, size_t prefix, const FixtureMachODef *m) {
    int be = m->big_endian ? 1 : 0;
    int is64 = m->bitness == 64;
    uint32_t hdr = is64 ? 32u : 28u;
    uint32_t seghdr = is64 ? 72u : 56u;
    uint32_t sectsz = is64 ? 80u : 68u;
    uint32_t nlsz = is64 ? 16u : 12u;
    uint32_t i, j, ngroups = 0, ncmds, sizeofcmds = 0, strneed = 1;
    uint32_t *strxs = NULL, strsize, cmd_end, symoff = 0, stroff = 0, isymoff = 0, filesize;
    uint64_t end;
    unsigned char *b, *p;
    size_t o;
    uint32_t sec_i;
    FILE *f;
    int ok;

    if (m->bitness != 32 && m->bitness != 64) return 1;

    for (i=0;i<m->nsections;i++)
        if (i==0 || strcmp(m->sections[i].seg, m->sections[i-1].seg)!=0)
            ngroups++;

    if (m->nsymbols) {
        strxs = (uint32_t*)calloc(m->nsymbols, sizeof(uint32_t));
        if (!strxs) return 1;
        for (i=0;i<m->nsymbols;i++) {
            if (m->symbols[i].name) {
                strxs[i] = strneed;
                strneed += (uint32_t)strlen(m->symbols[i].name) + 1;
            } else {
                strxs[i] = m->symbols[i].strx;
            }
        }
    }
    strsize = m->strsize ? m->strsize : strneed;

    for (i=0;i<m->nsections;i++) {
        if (i==0 || strcmp(m->sections[i].seg, m->sections[i-1].seg)!=0) {
            uint32_t gcount = 1;
            for (j=i+1;j<m->nsections && strcmp(m->sections[j].seg, m->sections[i].seg)==0;j++) gcount++;
            sizeofcmds += seghdr + gcount*sectsz;
        }
    }
    if (m->nsymbols) sizeofcmds += 24;
    if (m->nindirect) sizeofcmds += 80;
    for (i=0;i<m->ndylibs;i++)
        sizeofcmds += fixture_align_up(24 + (uint32_t)strlen(m->dylibs[i].name) + 1, 8);

    ncmds = ngroups + (m->nsymbols?1u:0u) + (m->nindirect?1u:0u) + m->ndylibs;
    cmd_end = hdr + sizeofcmds;

    symoff = m->symoff ? m->symoff : (m->nsymbols ? fixture_align_up(cmd_end, 8) : 0);
    stroff = m->stroff ? m->stroff : (m->nsymbols ? symoff + m->nsymbols*nlsz : 0);
    isymoff = m->indirectsymoff ? m->indirectsymoff : (m->nindirect ? stroff + strsize : 0);

    end = cmd_end;
    if (m->nsymbols) {
        uint64_t e1 = (uint64_t)symoff + (uint64_t)m->nsymbols*nlsz;
        uint64_t e2 = (uint64_t)stroff + strsize;
        if (e1 > end) end = e1;
        if (e2 > end) end = e2;
    }
    if (m->nindirect) {
        uint64_t e3 = (uint64_t)isymoff + (uint64_t)m->nindirect*4u;
        if (e3 > end) end = e3;
    }
    for (i=0;i<m->nsections;i++) {
        uint64_t e4 = (uint64_t)m->sections[i].offset + m->sections[i].size;
        if (e4 > end) end = e4;
    }
    filesize = m->filesize ? m->filesize : fixture_align_up((uint32_t)end, 16);
    if (filesize < cmd_end) { free(strxs); return 1; }

    b = (unsigned char*)calloc(1, (size_t)prefix + filesize);
    if (!b) { free(strxs); return 1; }
    for (i=0;i<prefix;i++) b[i]=0xa5;
    p = b + prefix;

    fixture_u32e(p, 0, is64 ? 0xfeedfacfu : 0xfeedfaceu, be);
    fixture_u32e(p, 4, is64 ? 0x01000007u : 0x00000007u, be);
    fixture_u32e(p, 8, 3, be);
    fixture_u32e(p, 12, m->filetype, be);
    fixture_u32e(p, 16, ncmds, be);
    fixture_u32e(p, 20, sizeofcmds, be);
    fixture_u32e(p, 24, m->flags, be);

    o = hdr;
    sec_i = 0;
    while (sec_i < m->nsections) {
        uint32_t gcount = 1, k;
        uint64_t vmaddr, vend, foff, fend;
        for (j=sec_i+1;j<m->nsections && strcmp(m->sections[j].seg, m->sections[sec_i].seg)==0;j++) gcount++;
        vmaddr = m->sections[sec_i].addr; vend = m->sections[sec_i].addr + m->sections[sec_i].size;
        foff = m->sections[sec_i].offset; fend = (uint64_t)m->sections[sec_i].offset + m->sections[sec_i].size;
        for (k=1;k<gcount;k++) {
            const FixtureSectionDef *s = &m->sections[sec_i+k];
            if (s->addr < vmaddr) vmaddr = s->addr;
            if (s->addr + s->size > vend) vend = s->addr + s->size;
            if (s->offset < foff) foff = s->offset;
            if ((uint64_t)s->offset + s->size > fend) fend = (uint64_t)s->offset + s->size;
        }
        fixture_u32e(p, o, is64 ? FIXTURE_LC_SEGMENT_64 : FIXTURE_LC_SEGMENT, be);
        fixture_u32e(p, o+4, seghdr + gcount*sectsz, be);
        fixture_name16(p, o+8, m->sections[sec_i].seg);
        if (is64) {
            fixture_u64e(p, o+24, vmaddr, be);
            fixture_u64e(p, o+32, vend - vmaddr, be);
            fixture_u64e(p, o+40, foff, be);
            fixture_u64e(p, o+48, fend - foff, be);
            fixture_u32e(p, o+56, 7, be);
            fixture_u32e(p, o+60, 5, be);
            fixture_u32e(p, o+64, gcount, be);
        } else {
            fixture_u32e(p, o+24, (uint32_t)vmaddr, be);
            fixture_u32e(p, o+28, (uint32_t)(vend - vmaddr), be);
            fixture_u32e(p, o+32, (uint32_t)foff, be);
            fixture_u32e(p, o+36, (uint32_t)(fend - foff), be);
            fixture_u32e(p, o+40, 7, be);
            fixture_u32e(p, o+44, 5, be);
            fixture_u32e(p, o+48, gcount, be);
        }
        for (k=0;k<gcount;k++) {
            const FixtureSectionDef *s = &m->sections[sec_i+k];
            size_t t = o + seghdr + (size_t)k*sectsz;
            fixture_name16(p, t, s->name);
            fixture_name16(p, t+16, s->seg);
            if (is64) {
                fixture_u64e(p, t+32, s->addr, be);
                fixture_u64e(p, t+40, s->size, be);
                fixture_u32e(p, t+48, s->offset, be);
                fixture_u32e(p, t+52, 3, be);
                fixture_u32e(p, t+64, s->flags, be);
                fixture_u32e(p, t+68, s->reserved1, be);
                fixture_u32e(p, t+72, s->reserved2, be);
            } else {
                fixture_u32e(p, t+32, (uint32_t)s->addr, be);
                fixture_u32e(p, t+36, (uint32_t)s->size, be);
                fixture_u32e(p, t+40, s->offset, be);
                fixture_u32e(p, t+44, 3, be);
                fixture_u32e(p, t+56, s->flags, be);
                fixture_u32e(p, t+60, s->reserved1, be);
                fixture_u32e(p, t+64, s->reserved2, be);
            }
        }
        o += seghdr + gcount*sectsz;
        sec_i += gcount;
    }
    if (m->nsymbols) {
        fixture_u32e(p, o, FIXTURE_LC_SYMTAB, be);
        fixture_u32e(p, o+4, 24, be);
        fixture_u32e(p, o+8, symoff, be);
        fixture_u32e(p, o+12, m->nsymbols, be);
        fixture_u32e(p, o+16, stroff, be);
        fixture_u32e(p, o+20, strsize, be);
        o += 24;
    }
    if (m->nindirect) {
        fixture_u32e(p, o, FIXTURE_LC_DYSYMTAB, be);
        fixture_u32e(p, o+4, 80, be);
        fixture_u32e(p, o+28, m->nsymbols, be); /* nundefsym */
        fixture_u32e(p, o+56, isymoff, be);
        fixture_u32e(p, o+60, m->nindirect, be);
        o += 80;
    }
    for (i=0;i<m->ndylibs;i++) {
        uint32_t nl = (uint32_t)strlen(m->dylibs[i].name) + 1;
        uint32_t cs = fixture_align_up(24 + nl, 8);
        fixture_u32e(p, o, m->dylibs[i].cmd, be);
        fixture_u32e(p, o+4, cs, be);
        fixture_u32e(p, o+8, 24, be);       /* name offset */
        fixture_u32e(p, o+12, 2, be);       /* timestamp */
        fixture_u32e(p, o+16, 0x10000, be); /* current version 1.0.0 */
        fixture_u32e(p, o+20, 0x10000, be); /* compatibility version */
        memcpy(p+o+24, m->dylibs[i].name, nl);
        o += cs;
    }
    for (i=0;i<m->nsymbols;i++) {
        size_t e = (size_t)symoff + (size_t)i*nlsz;
        if (e + nlsz > filesize) continue;
        fixture_u32e(p, e, strxs[i], be);
        p[e+4] = m->symbols[i].type;
        p[e+5] = m->symbols[i].sect;
        fixture_u16e(p, e+6, m->symbols[i].desc, be);
        if (is64) fixture_u64e(p, e+8, m->symbols[i].value, be);
        else fixture_u32e(p, e+8, (uint32_t)m->symbols[i].value, be);
    }
    if (m->nsymbols) {
        for (i=0;i<m->nsymbols;i++) {
            if (!m->symbols[i].name) continue;
            if ((uint64_t)strxs[i] >= strsize) continue;
            {
                uint32_t nl = (uint32_t)strlen(m->symbols[i].name) + 1;
                uint32_t room = strsize - strxs[i];
                if (nl > room) nl = room;
                if ((uint64_t)stroff + strxs[i] + nl > filesize) continue;
                memcpy(p + stroff + strxs[i], m->symbols[i].name, nl);
            }
        }
    }
    for (i=0;i<m->nindirect;i++) {
        size_t e = (size_t)isymoff + (size_t)i*4u;
        if (e + 4 > filesize) continue;
        fixture_u32e(p, e, m->indirect[i], be);
    }

    f = fopen(path, "wb");
    if (!f) { free(b); free(strxs); return 1; }
    ok = fwrite(b, 1, (size_t)prefix + filesize, f) == (size_t)prefix + filesize;
    if (fclose(f) != 0) ok = 0;
    free(b);
    free(strxs);
    return ok ? 0 : 1;
}
#endif
