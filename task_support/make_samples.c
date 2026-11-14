/* Legal, deterministic ELF64 ET_REL objects and GNU archive metadata only. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
static void put(unsigned char *p,uint64_t v,int n){int i;for(i=0;i<n;i++)p[i]=(unsigned char)(v>>(8*i));}
static void be32(unsigned char *p,uint32_t v){int i;for(i=0;i<4;i++)p[i]=(unsigned char)(v>>(24-8*i));}
static void write_file(const char *p,const unsigned char *b,size_t n){FILE*f=fopen(p,"wb");if(!f||fwrite(b,1,n,f)!=n||fclose(f))exit(2);}
static void object(unsigned char *b,const char *name){
 unsigned char*h;const char names[]="\0.text\0.strtab\0.symtab\0.shstrtab\0";size_t len=strlen(name);
 memset(b,0,512);memcpy(b,"\177ELF\2\1\1",7);put(b+16,1,2);put(b+18,62,2);put(b+20,1,4);put(b+40,192,8);put(b+52,64,2);put(b+58,64,2);put(b+60,5,2);put(b+62,4,2);
 memcpy(b+69,name,len+1);put(b+104,1,4);b[108]=0x12;put(b+110,1,2);put(b+120,4,8);memcpy(b+128,names,sizeof(names));
 h=b+256;put(h,1,4);put(h+4,1,4);put(h+8,6,8);put(h+24,64,8);put(h+32,4,8);put(h+48,1,8);
 h=b+320;put(h,7,4);put(h+4,3,4);put(h+24,68,8);put(h+32,len+2,8);put(h+48,1,8);
 h=b+384;put(h,15,4);put(h+4,2,4);put(h+24,80,8);put(h+32,48,8);put(h+40,2,4);put(h+44,1,4);put(h+48,8,8);put(h+56,24,8);
 h=b+448;put(h,23,4);put(h+4,3,4);put(h+24,128,8);put(h+32,sizeof(names),8);put(h+48,1,8);
}
static void header(FILE*f,const char *name,size_t n){if(fprintf(f,"%-16s%-12d%-6d%-6d%-8o%-10zu`\n",name,0,0,0,0100644,n)!=60)exit(3);}
static void payload(FILE*f,const unsigned char*b,size_t n){if(fwrite(b,1,n,f)!=n)exit(3);if(n&1)fputc('\n',f);}
static void archive(const char*path,int thin,const unsigned char*a,const unsigned char*b){
 const char n1[]="objects/first-object.o/\n",n2[]="objects/second-object.o/\n";
 unsigned char names[128],syms[23]={0};char ref[16];size_t ns=sizeof(n1)-1+sizeof(n2)-1;
 size_t first=8+60+24+60+((ns+1)&~(size_t)1),second=first+60+(thin?0:512);
 FILE*f=fopen(path,"wb");if(!f)exit(3);memcpy(names,n1,sizeof(n1)-1);memcpy(names+sizeof(n1)-1,n2,sizeof(n2)-1);
 be32(syms,2);be32(syms+4,(uint32_t)first);be32(syms+8,(uint32_t)second);memcpy(syms+12,"alpha\0beta\0",11);
 fwrite(thin?"!<thin>\n":"!<arch>\n",1,8,f);header(f,"/",sizeof(syms));payload(f,syms,sizeof(syms));header(f,"//",ns);payload(f,names,ns);
 header(f,"/0",512);if(!thin)payload(f,a,512);snprintf(ref,sizeof(ref),"/%zu",sizeof(n1)-1);header(f,ref,512);if(!thin)payload(f,b,512);
 if(fclose(f))exit(3);printf("FIXTURE %s first=%zu second=%zu object_size=512\n",path,first,second);
}
int main(void){unsigned char a[512],b[512];mkdir("task_support/samples",0755);mkdir("task_support/samples/objects",0755);object(a,"alpha");object(b,"beta");write_file("task_support/samples/objects/first-object.o",a,512);write_file("task_support/samples/objects/second-object.o",b,512);archive("task_support/samples/regular.a",0,a,b);archive("task_support/samples/thin.a",1,a,b);return 0;}
