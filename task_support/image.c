/* Legal, hand-encoded MSF7 material, with no executable program attached. */
#include "image.h"
#include <stdlib.h>
#include <string.h>
static void u16(unsigned char *p, uint16_t x){p[0]=x;p[1]=x>>8;}
static void u32(unsigned char *p, uint32_t x){for(int i=0;i<4;i++)p[i]=(unsigned char)(x>>(8*i));}
static unsigned align4(unsigned n){return (n+3)&~3u;}
static unsigned procedure(unsigned char *p,int local,unsigned addr){
 const char *name="shared_name";unsigned n=align4(39+(unsigned)strlen(name)+1);
 memset(p,0,n);u16(p,n-2);u16(p+2,local?0x110f:0x1110);
 u32(p+8,4+n);u32(p+16,0x20);u32(p+24,0x20);u32(p+28,0x74);
 u32(p+32,addr);u16(p+36,1);memcpy(p+39,name,strlen(name)+1);return n;
}
static unsigned module_info(unsigned char *p,unsigned stream,unsigned symbytes,const char *obj){
 const char *name="same.obj";unsigned n=align4(64+(unsigned)strlen(name)+1+(unsigned)strlen(obj)+1);
 memset(p,0,n);u16(p+4,1);u32(p+8,0x20+(stream-8)*0x60);u32(p+12,0x20);u16(p+20,stream-8);u16(p+34,stream);
 u32(p+36,symbytes);u32(p+44,12);
 memcpy(p+64,name,strlen(name)+1);memcpy(p+64+strlen(name)+1,obj,strlen(obj)+1);return n;
}
static unsigned reference(unsigned char *p,int local,unsigned module){
 const char *name="shared_name";unsigned n=align4(14+(unsigned)strlen(name)+1);
 memset(p,0,n);u16(p,n-2);u16(p+2,local?0x1127:0x1125);u32(p+8,4);u16(p+12,module);
 memcpy(p+14,name,strlen(name)+1);return n;
}
static unsigned public_record(unsigned char *p){
 const char *name="public_marker";unsigned n=align4(14+(unsigned)strlen(name)+1);
 memset(p,0,n);u16(p,n-2);u16(p+2,0x110e);u32(p+4,2);u32(p+8,0x20);u16(p+12,1);
 memcpy(p+14,name,strlen(name)+1);return n;
}
static unsigned name_hash(const char *s){
 uint32_t h=0;unsigned n=(unsigned)strlen(s),i=0;
 while(n-i>=4){uint32_t v=0;for(unsigned j=0;j<4;j++)v|=(uint32_t)(unsigned char)s[i+j]<<(8*j);h^=v;i+=4;}
 if(n-i>=2){h^=(unsigned char)s[i]|((uint32_t)(unsigned char)s[i+1]<<8);i+=2;}
 if(n-i)h^=(unsigned char)s[i];h|=0x20202020;h^=h>>11;return (h^(h>>16))%4096;
}
int task_make_image(struct task_image *out,int mode){
 unsigned char streams[11][2048]={{0}};unsigned sizes[11]={0};unsigned symbytes[2];
 memset(out,0,sizeof(*out));if(mode<0||mode>2)return 1;
 u32(streams[1],20000404);u32(streams[1]+8,7);u32(streams[1]+36,1);sizes[1]=48;
 for(int i=0;i<2;i++){
  unsigned char *s=streams[8+i];u32(s,4);unsigned n=procedure(s+4,i,0x20+i*0x60);
  u16(s+4+n,2);u16(s+6+n,6);symbytes[i]=4+n+4;
  u32(s+symbytes[i],0xf3);u32(s+symbytes[i]+4,4);s[symbytes[i]+9]='A';
  sizes[8+i]=symbytes[i]+12+4;out->procedure_offsets[i]=4;
 }
 unsigned char *d=streams[3];u32(d,0xffffffff);u32(d+4,19990903);u32(d+8,7);
 u16(d+12,mode?0xffff:10);u16(d+16,mode?0xffff:6);u16(d+20,mode==2?0xffff:5);
 unsigned modsize=module_info(d+64,8,symbytes[0],"dirA/same.obj");
 modsize+=module_info(d+64+modsize,9,symbytes[1],"dirB/same.obj");
 u32(d+24,modsize);u32(d+32,4);u32(d+48,22);u16(d+58,0x8664);
 unsigned debug=64+modsize+4;for(int i=0;i<11;i++)u16(d+debug+i*2,0xffff);u16(d+debug+10,7);
 sizes[3]=debug+22;
 unsigned first=reference(streams[5],0,1);out->reference_offsets[0]=0;out->reference_offsets[1]=first;
 unsigned puboff=first+reference(streams[5]+first,1,2);
 sizes[5]=mode==2?0:puboff+public_record(streams[5]+puboff);
 /* One public symbol in the ordinary PSI; no dependence on a target resolver. */
 sizes[6]=mode?0:572;u32(streams[6],544);u32(streams[6]+28,0xffffffff);
 u32(streams[6]+32,0xf12f091a);u32(streams[6]+36,8);u32(streams[6]+40,520);
 u32(streams[6]+44,puboff+1);u32(streams[6]+48,1);
 unsigned hash=name_hash("public_marker");streams[6][52+hash/8]|=(unsigned char)(1u<<(hash%8));
 memcpy(streams[7],".text",5);u32(streams[7]+8,0x200);u32(streams[7]+12,0x1000);sizes[7]=40;
 sizes[10]=mode?0:552;u32(streams[10],0xffffffff);u32(streams[10]+4,0xf12f091a);
 u32(streams[10]+8,16);u32(streams[10]+12,520);
 u32(streams[10]+16,1);u32(streams[10]+20,1);u32(streams[10]+24,first+1);u32(streams[10]+28,1);
 unsigned refhash=name_hash("shared_name");streams[10][32+refhash/8]|=(unsigned char)(1u<<(refhash%8));
 const unsigned block=512;unsigned pages=0;
 for(int i=0;i<11;i++)pages+=(sizes[i]+block-1)/block;
 unsigned dirsize=4+44+pages*4;unsigned blocks=5+pages;
 unsigned char *data=calloc(blocks,block);if(!data)return 1;
 memcpy(data,"Microsoft C/C++ MSF 7.00\r\n\x1a\x44\x53\0\0\0",32);
 u32(data+32,block);u32(data+36,1);u32(data+40,blocks);u32(data+44,dirsize);u32(data+52,3);
 memset(data+block,0xff,block);memset(data+2*block,0xff,block);
 u32(data+3*block,4);unsigned char *dir=data+4*block;u32(dir,11);unsigned k=5,di=48;
 for(int i=0;i<11;i++)u32(dir+4+i*4,sizes[i]);
 for(int i=0;i<11;i++)for(unsigned pos=0;pos<sizes[i];pos+=block){
  u32(dir+di,k);di+=4;unsigned n=sizes[i]-pos;if(n>block)n=block;
  memcpy(data+k*block,streams[i]+pos,n);k++;
 }
 /* Mark allocated blocks; padding bits remain free. */
 for(unsigned i=0;i<blocks;i++){data[block+i/8]&=(unsigned char)~(1u<<(i%8));data[2*block+i/8]&=(unsigned char)~(1u<<(i%8));}
 out->data=data;out->size=(size_t)blocks*block;return 0;
}
void task_free_image(struct task_image *image){free(image->data);image->data=NULL;image->size=0;}
