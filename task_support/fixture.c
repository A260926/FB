/* Original synthetic metadata fixture; no executable code is emitted. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned char b[4096];
static void put(unsigned at, uint64_t n, unsigned bytes) {
    unsigned i; for (i=0;i<bytes;++i) b[at+i]=(unsigned char)(n>>(8*i));
}
int main(int argc,char **argv) {
    unsigned f=128,p=f,c,fs,n,sh;
    const char names[]="\0.shstrtab\0.debug_frame\0";
    if(argc!=3) return 2;
    memcpy(b,"\177ELF\2\1\1",7); put(16,1,2); put(18,183,2); put(20,1,4);
    put(52,64,2);put(58,64,2);put(60,3,2);put(62,1,2);
    memcpy(b+64,names,sizeof(names));
    c=p;p+=4;put(p,0xffffffffu,4);p+=4;
    b[p++]=3;b[p++]=0; /* version 3, empty augmentation */
    b[p++]=4;b[p++]=0x78;b[p++]=30; /* align 4, -8, LR */
    b[p++]=0x0c;b[p++]=31;b[p++]=16; /* CFA=SP+16 */
    /* Legal CIE nop padding; keeps the fixture clear of an unrelated
       upstream minimum-record-count heuristic in dwarf_frame2.c. */
    while(p-c<40)b[p++]=0;
    while((p-c)%4)b[p++]=0;
    put(c,p-c-4,4);
    fs=p;p+=4;put(p,c-f,4);p+=4;
    put(p,0x1000,8);p+=8;put(p,0x40,8);p+=8;
    if(!strcmp(argv[2],"plain")) {
        b[p++]=0x44;b[p++]=0x0e;b[p++]=32;
    } else if(!strcmp(argv[2],"negate")) {
        b[p++]=0x41;b[p++]=0x2d;b[p++]=0x41;b[p++]=0x2d;
    } else if(!strcmp(argv[2],"set")) {
        const unsigned char op[]={0x41,0x2b,2,0x7f,0x41,0x2b,0,0};
        memcpy(b+p,op,sizeof(op));p+=sizeof(op);
    } else return 3;
    while((p-fs)%4)b[p++]=0;
    put(fs,p-fs-4,4);n=p-f;
    sh=(p+7u)&~7u;put(40,sh,8);
    /* Null section, string table, debug_frame. */
    put(sh+64,1,4);put(sh+64+4,3,4);put(sh+64+24,64,8);
    put(sh+64+32,sizeof(names),8);put(sh+64+48,1,8);
    put(sh+128,11,4);put(sh+128+4,1,4);put(sh+128+24,f,8);
    put(sh+128+32,n,8);put(sh+128+48,8,8);
    FILE *out=fopen(argv[1],"wb"); if(!out)return 4;
    int failed=fwrite(b,1,sh+192,out)!=sh+192;
    return fclose(out)||failed;
}
