#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <yara.h>
#include <yara/dotnet.h>
static unsigned char data[2048];
static void w16(int p,unsigned v){data[p]=v;data[p+1]=v>>8;}
static void w32(int p,unsigned v){w16(p,v);w16(p+2,v>>16);}
static int cb(YR_SCAN_CONTEXT*c,int msg,void*d,void*u){(void)c;(void)d;if(msg==CALLBACK_MSG_RULE_MATCHING)(*(int*)u)++;return CALLBACK_CONTINUE;}
int main(void){
  w16(0,0x5a4d);w32(0x3c,0x80);w32(0x80,0x4550);
  w16(0x84,0x14c);w16(0x86,1);w16(0x94,224);w16(0x96,0x102);
  w16(0x98,0x10b);w32(0x98+28,0x400000);w32(0x98+32,0x1000);w32(0x98+36,0x200);
  w32(0x98+56,0x2000);w32(0x98+60,0x200);w32(0x98+92,16);
  w32(0x98+96+14*8,0x1000);w32(0x98+96+14*8+4,72);
  memcpy(data+0x178,".meta",5);w32(0x180,0x600);w32(0x184,0x1000);w32(0x188,0x600);w32(0x18c,0x200);
  w32(0x200,72);w16(0x204,2);w32(0x208,0x1080);w32(0x20c,0x280);w32(0x210,1);
  w32(0x280,0x424a5342);w16(0x284,1);w32(0x28c,12);memcpy(data+0x290,"v4.0.30319",10);w16(0x29e,3);
  w32(0x2a0,0x80);w32(0x2a4,30);memcpy(data+0x2a8,"#~",3);
  w32(0x2ac,0xc0);w32(0x2b0,32);memcpy(data+0x2b4,"#Strings",9);
  w32(0x2c0,0x100);w32(0x2c4,8);memcpy(data+0x2c8,"#Blob",6);
  data[0x304]=2;w32(0x308,1u<<26);w32(0x318,1);w16(0x31c,1);memcpy(data+0x341,"benign-native",14);
  YR_COMPILER*c=NULL;YR_RULES*r=NULL;int matches=0;
  assert(yr_initialize()==0);assert(yr_compiler_create(&c)==0);
  assert(yr_compiler_add_string(c,"import \"dotnet\" rule local { condition: dotnet.is_dotnet and dotnet.number_of_modulerefs == 1 and dotnet.modulerefs[0] == \"benign-native\" }",NULL)==0);
  assert(yr_compiler_get_rules(c,&r)==0);assert(yr_rules_scan_mem(r,data,sizeof(data),0,cb,&matches,0)==0);assert(matches==1);
  yr_rules_destroy(r);yr_compiler_destroy(c);assert(yr_finalize()==0);
  puts("YARA_DOTNET_MODULEREF_OK");return 0;
}
