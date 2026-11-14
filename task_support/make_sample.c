#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static unsigned char f[0x3200],tab[2048],str[256],blob[128],meta[4096];
static size_t nt,ns,nb,nm;
static void p16(unsigned char *p,uint16_t x){p[0]=x;p[1]=x>>8;}
static void p32(unsigned char *p,uint32_t x){for(int i=0;i<4;i++)p[i]=x>>(8*i);}
static void t16(uint16_t v){p16(tab+nt,v);nt+=2;}
static void t32(uint32_t v){p32(tab+nt,v);nt+=4;}
static unsigned s(const char *v){unsigned n=ns;size_t z=strlen(v)+1;memcpy(str+ns,v,z);ns+=z;return n;}
static void sec(size_t p,const char *name,uint32_t sz,uint32_t rva,uint32_t raw,uint32_t flags){memcpy(f+p,name,strlen(name));p32(f+p+8,sz);p32(f+p+12,rva);p32(f+p+16,sz);p32(f+p+20,raw);p32(f+p+36,flags);}
static void td(uint32_t flags,unsigned name,unsigned space,unsigned ext,unsigned methods){t32(flags);t16(name);t16(space);t16(ext);t16(1);t16(methods);}
static void md(uint32_t rva,unsigned flags,unsigned name,unsigned sig,unsigned params){t32(rva);t16(0);t16(flags);t16(name);t16(sig);t16(params);}
static void param(unsigned seq,unsigned name){t16(0);t16(seq);t16(name);}
static void stream(size_t *h,const char *name,size_t offset,size_t n){p32(meta+*h,offset);p32(meta+*h+4,n);strcpy((char*)meta+*h+8,name);*h+=8+((strlen(name)+1+3)&~(size_t)3);}
static int make(const char *path,int ptr){
 memset(f,0,sizeof(f));memset(tab,0,sizeof(tab));memset(str,0,sizeof(str));memset(blob,0,sizeof(blob));memset(meta,0,sizeof(meta));nt=24;ns=1;nb=1;
 unsigned module=s("sample.netmodule"),global=s("<Module>"),outer=s("Outer"),inner=s("Inner"),space=s("Demo"),object=s("Object"),system=s("System"),mscorlib=s("mscorlib");
 unsigned load=s("Load"),store=s("Store"),stub=s("Stub"),a=s("a"),b=s("b"),result=s("result");
 unsigned sig=nb;blob[nb++]=4;blob[nb++]=0;blob[nb++]=1;blob[nb++]=8;blob[nb++]=8;
 unsigned sig0=nb;blob[nb++]=3;blob[nb++]=0x20;blob[nb++]=0;blob[nb++]=8;
 uint64_t valid=((uint64_t)1<<0)|((uint64_t)1<<1)|((uint64_t)1<<2)|((uint64_t)1<<6)|((uint64_t)1<<8)|((uint64_t)1<<35)|((uint64_t)1<<41);
 if(ptr)valid|=((uint64_t)1<<5)|((uint64_t)1<<7);
 tab[4]=2;tab[7]=1;for(int i=0;i<8;i++)tab[8+i]=valid>>(8*i);
 for(unsigned id=0;id<64;id++)if((valid>>id)&1)t32((id==2||id==5||id==6||id==7||id==8)?3:1);
 /* Module; TypeRef(System.Object); three TypeDef rows. */
 t16(0);t16(module);t16(1);t16(0);t16(0);
 t16(6);t16(object);t16(system);
 td(0,global,0,0,1);td(0x81,outer,space,5,1);td(2,inner,0,5,2);
 if(ptr){t16(3);t16(1);t16(2);}
 /* Physical MethodDef order intentionally differs between #- and #~. */
 if(ptr){md(0x4000,0x16,load,sig,1);md(0x4ff8,0x16,store,sig,3);md(0,0x446,stub,sig0,4);}
 else{md(0,0x446,stub,sig0,1);md(0x4000,0x16,load,sig,1);md(0x4ff8,0x16,store,sig,3);}
 if(ptr){t16(2);t16(1);t16(3);param(1,a);param(0,result);param(1,b);}
 else{param(0,result);param(1,a);param(1,b);}
 /* AssemblyRef without a public-key token; no cryptographic material. */
 t16(4);t16(0);t16(0);t16(0);t32(0);t16(0);t16(mscorlib);t16(0);t16(0);
 t16(3);t16(2); /* NestedClass */
 p32(meta,0x424a5342);p16(meta+4,1);p16(meta+6,1);p32(meta+12,12);memcpy(meta+16,"v4.0.30319",11);p16(meta+30,4);
 size_t h=32,off=128;stream(&h,ptr?"#-":"#~",off,nt);memcpy(meta+off,tab,nt);off=(off+nt+3)&~(size_t)3;
 stream(&h,"#Strings",off,ns);memcpy(meta+off,str,ns);off=(off+ns+3)&~(size_t)3;
 stream(&h,"#Blob",off,nb);memcpy(meta+off,blob,nb);off=(off+nb+3)&~(size_t)3;
 stream(&h,"#GUID",off,16);for(unsigned i=0;i<16;i++)meta[off+i]=(unsigned char)(i+1);nm=off+16;
 p16(f,0x5a4d);p32(f+0x3c,0x80);p32(f+0x80,0x4550);p16(f+0x84,0x14c);p16(f+0x86,3);p16(f+0x94,224);p16(f+0x96,0x2102);
 size_t o=0x98;p16(f+o,0x10b);p32(f+o+28,0x400000);p32(f+o+32,0x1000);p32(f+o+36,0x200);p32(f+o+56,0x6000);p32(f+o+60,0x400);p16(f+o+68,3);p32(f+o+92,16);p32(f+o+96+14*8,0x2000);p32(f+o+96+14*8+4,72);
 sec(0x178,".meta",0x800,0x2000,0x400,0x40000040);sec(0x1a0,".il1",0x1000,0x4000,0x1000,0x60000020);sec(0x1c8,".il2",0x200,0x5000,0x3000,0x60000020);
 p32(f+0x400,72);p16(f+0x404,2);p16(f+0x406,5);p32(f+0x408,0x2100);p32(f+0x40c,nm);p32(f+0x410,1);memcpy(f+0x500,meta,nm);
 f[0x1000]=(3<<2)|2;f[0x1001]=2;f[0x1002]=0;f[0x1003]=0x2a;
 unsigned char fat[18]={3,0x30,2,0,6,0,0,0,0,0,0,0,2,0x17,0x58,0,0,0x2a};
 memcpy(f+0x1ff8,fat,8);memcpy(f+0x3000,fat+8,10);
 FILE *out=fopen(path,"wb");if(!out)return 1;int ok=fwrite(f,1,sizeof(f),out)==sizeof(f);if(fclose(out))return 2;return ok?0:3;
}
int main(void){if(make(".task-build/plain.exe",0)||make(".task-build/ptr.exe",1))return 1;puts("SYNTHETIC_CLR_SAMPLES=2");return 0;}
