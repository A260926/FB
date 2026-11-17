/* Synthetic, inert analysis data. No instruction stream is executed. */
#include "fixture.h"
#include <stdio.h>
#include <string.h>
static void put(unsigned char *p, uint64_t v, unsigned n, int be) {
    unsigned i;
    for (i = 0; i < n; ++i) p[be ? n-1-i : i] = (unsigned char)(v >> (i*8));
}
static void shdr(unsigned char *p, int bits, int be, unsigned name, unsigned type,
                 uint64_t flags, uint64_t off, uint64_t size, unsigned link,
                 unsigned info, unsigned align, unsigned entsize) {
    put(p,name,4,be); put(p+4,type,4,be);
    if (bits == 64) {
        put(p+8,flags,8,be); put(p+24,off,8,be); put(p+32,size,8,be);
        put(p+40,link,4,be); put(p+44,info,4,be); put(p+48,align,8,be); put(p+56,entsize,8,be);
    } else {
        put(p+8,flags,4,be); put(p+16,off,4,be); put(p+20,size,4,be);
        put(p+24,link,4,be); put(p+28,info,4,be); put(p+32,align,4,be); put(p+36,entsize,4,be);
    }
}
size_t fixture_elf(unsigned char *p, size_t cap, int bits, int be, uint64_t value) {
    unsigned hs = bits == 64 ? 64 : 52, ss = bits == 64 ? 64 : 40, syms = bits == 64 ? 24 : 16;
    static const char names[] = "\0.text\0.strtab\0.symtab\0.shstrtab\0";
    unsigned char *s;
    if (cap < 768 || (bits != 32 && bits != 64)) return 0;
    memset(p,0,768); memcpy(p,"\177ELF",4); p[4] = bits == 64 ? 2 : 1; p[5] = be ? 2 : 1; p[6] = 1;
    put(p+16,1,2,be); put(p+18,bits == 64 ? 62 : 20,2,be); put(p+20,1,4,be);
    if (bits == 64) {
        put(p+40,0x180,8,be); put(p+52,hs,2,be); put(p+54,56,2,be);
        put(p+58,ss,2,be); put(p+60,5,2,be); put(p+62,4,2,be);
    } else {
        put(p+32,0x180,4,be); put(p+40,hs,2,be); put(p+42,32,2,be);
        put(p+46,ss,2,be); put(p+48,5,2,be); put(p+50,4,2,be);
    }
    memcpy(p+0x90,"\0same\0",6); memcpy(p+0xa0,names,sizeof(names));
    s = p+0xc0+syms; put(s,1,4,be);
    if (bits == 64) { s[4] = 0x11; put(s+6,1,2,be); put(s+8,value,8,be); put(s+16,1,8,be); }
    else { put(s+4,value,4,be); put(s+8,1,4,be); s[12] = 0x11; put(s+14,1,2,be); }
    shdr(p+0x180+ss,bits,be,1,1,6,0x80,8,0,0,1,0);
    shdr(p+0x180+ss*2,bits,be,7,3,0,0x90,6,0,0,1,0);
    shdr(p+0x180+ss*3,bits,be,15,2,0,0xc0,syms*2,2,1,bits==64?8:4,syms);
    shdr(p+0x180+ss*4,bits,be,23,3,0,0xa0,sizeof(names),0,0,1,0);
    return 768;
}
size_t fixture_fat(unsigned char *p, size_t cap) {
    unsigned char *r;
    if (cap < 8960) return 0;
    memset(p,0,8960); put(p,0x1f0e70fa,4,0); put(p+4,1,2,0); p[6]=2;
    r=p+8; put(r,62,2,0); r[4]=2; r[5]=1; put(r+8,4096,8,0); put(r+16,768,8,0);
    r=p+32; put(r,20,2,0); r[4]=1; r[5]=2; put(r+8,8192,8,0); put(r+16,768,8,0);
    if (!fixture_elf(p+4096,768,64,0,1) || !fixture_elf(p+8192,768,32,1,2)) return 0;
    return 8960;
}
static size_t member(unsigned char *p, const char *name, const unsigned char *data, size_t size) {
    char num[32]; size_t n;
    memset(p,' ',60); memcpy(p,name,strlen(name));
    p[16]='0'; p[28]='0'; p[34]='0'; memcpy(p+40,"100644",6);
    snprintf(num,sizeof(num),"%zu",size); memcpy(p+48,num,strlen(num)); p[58]='`'; p[59]='\n';
    memcpy(p+60,data,size); n=60+size; if (size&1) p[n++]='\n'; return n;
}
size_t fixture_archive(unsigned char *p, size_t cap, int nested) {
    unsigned char a[8960], b[768]; size_t n=8, as;
    if (cap < 10000) return 0;
    as = nested ? fixture_fat(a,sizeof(a)) : fixture_elf(a,sizeof(a),64,0,1);
    if (!as || !fixture_elf(b,sizeof(b),32,1,2)) return 0;
    memcpy(p,"!<arch>\n",8); n += member(p+n,nested?"bundle.fat/":"plain64.o/",a,as);
    n += member(p+n,"plain32.o/",b,sizeof(b)); return n;
}
