#ifndef TASK_MACHO_FIXTURE_H
#define TASK_MACHO_FIXTURE_H
#include <stdint.h>
#include <stdio.h>
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
#endif
