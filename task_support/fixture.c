/* Legal, synthetic ELF/DWARF data. This writer never executes its output. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char b[4096]; size_t n; } Buf;
static void put(Buf *b,uint64_t v,unsigned n) { while(n--) { b->b[b->n++]=(unsigned char)v; v>>=8; } }
static void patch(Buf *b,size_t at,uint64_t v,unsigned n) { while(n--) { b->b[at++]=(unsigned char)v; v>>=8; } }
static void uleb(Buf *b,uint64_t v) { do { unsigned c=v&127; v>>=7; put(b,c|(v?128:0),1); } while(v); }
static void str(Buf *b,const char *s) { size_t n=strlen(s)+1; memcpy(b->b+b->n,s,n); b->n+=n; }
static void abbrev(Buf *b,unsigned code,unsigned tag,unsigned children,const unsigned *pairs) {
    uleb(b,code);uleb(b,tag);put(b,children,1);
    do {uleb(b,pairs[0]);uleb(b,pairs[1]); if(!pairs[0])break; pairs+=2;}while(1);
}
static void sh(Buf *b,unsigned name,unsigned type,uint64_t flags,uint64_t addr,uint64_t off,uint64_t size) {
    put(b,name,4);put(b,type,4);put(b,flags,8);put(b,addr,8);put(b,off,8);put(b,size,8);
    put(b,0,4);put(b,0,4);put(b,1,8);put(b,0,8);
}
int main(int argc,char **argv) {
    Buf ab={{0},0},info={{0},0},file={{0},0},names={{0},0};
    unsigned cu[]={0x03,0x08,0,0};
    unsigned fn[]={0x03,0x08,0x11,0x01,0x12,0x06,0x7c,0x19,0,0};
    unsigned call[]={0x7d,0x01,0x7f,0x13,0,0};
    unsigned par[]={0x02,0x18,0x7e,0x18,0,0};
    size_t ref,callee,i,offinfo,offab,offnames,offtext,sht;
    unsigned ni,na,ns,nt;
    FILE *out;
    if(argc!=2)return 2;
    abbrev(&ab,1,0x11,1,cu);abbrev(&ab,2,0x2e,1,fn);
    abbrev(&ab,3,0x48,1,call);abbrev(&ab,4,0x49,0,par);
    abbrev(&ab,5,0x2e,0,fn);put(&ab,0,1);
    put(&info,0,4);put(&info,5,2);put(&info,1,1);put(&info,8,1);put(&info,0,4);
    put(&info,1,1);str(&info,"synthetic-call.c");
    put(&info,2,1);str(&info,"caller");put(&info,0x1000,8);put(&info,0x20,4);
    put(&info,3,1);put(&info,0x1010,8);ref=info.n;put(&info,0,4);
    put(&info,4,1);put(&info,1,1);put(&info,0x55,1);
    put(&info,3,1);put(&info,0x10,1);put(&info,7,1);put(&info,0x9f,1);
    put(&info,0,1);put(&info,0,1);
    callee=info.n;put(&info,5,1);str(&info,"callee");put(&info,0x2000,8);put(&info,0x10,4);
    put(&info,0,1);patch(&info,ref,callee,4);patch(&info,0,info.n-4,4);
    put(&names,0,1);ni=(unsigned)names.n;str(&names,".debug_info");
    na=(unsigned)names.n;str(&names,".debug_abbrev");ns=(unsigned)names.n;str(&names,".shstrtab");
    nt=(unsigned)names.n;str(&names,".text");
    put(&file,0x464c457f,4);put(&file,2,1);put(&file,1,1);put(&file,1,1);
    for(i=0;i<9;i++)put(&file,0,1);
    put(&file,2,2);put(&file,62,2);put(&file,1,4);put(&file,0x1000,8);put(&file,0,8);
    put(&file,0,8);put(&file,0,4);put(&file,64,2);put(&file,0,2);put(&file,0,2);
    put(&file,64,2);put(&file,5,2);put(&file,3,2);
    offinfo=file.n;memcpy(file.b+file.n,info.b,info.n);file.n+=info.n;
    offab=file.n;memcpy(file.b+file.n,ab.b,ab.n);file.n+=ab.n;
    offnames=file.n;memcpy(file.b+file.n,names.b,names.n);file.n+=names.n;
    offtext=file.n;for(i=0;i<32;i++)put(&file,0xc3,1);
    while(file.n%8)put(&file,0,1);
    sht=file.n;sh(&file,0,0,0,0,0,0);sh(&file,ni,1,0,0,offinfo,info.n);
    sh(&file,na,1,0,0,offab,ab.n);sh(&file,ns,3,0,0,offnames,names.n);
    sh(&file,nt,1,6,0x1000,offtext,32);patch(&file,40,sht,8);
    out=fopen(argv[1],"wb");if(!out)return 3;
    if(fwrite(file.b,1,file.n,out)!=file.n)return 4;
    if(fclose(out))return 5;
    printf("SYNTHETIC_ELF bytes=%zu cu=1 callsites=1\n",file.n);return 0;
}
