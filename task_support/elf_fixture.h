#ifndef ELF_FIXTURE_H
#define ELF_FIXTURE_H
#include <stdio.h>
#include "task_support/basic_fixture.h"
/* Simple ET_REL container writer for test data, never an ELF loader. */
static void putle(unsigned char *b,size_t at,uint64_t v,unsigned n){while(n--){b[at++]=(unsigned char)v;v>>=8;}}
static int write_basic_elf(const char *path,const struct dwarf_sections *s,const char *suffix){
 const char *names[]={".debug_info",".debug_line",".debug_abbrev",".debug_ranges",".debug_str",".debug_addr",".debug_str_offsets",".debug_line_str",".debug_rnglists"};
 unsigned char b[32768]={0};size_t n=64+64*(DEBUG_MAX+2),name_at[DEBUG_MAX],data_at[DEBUG_MAX];
 size_t strings=n;b[n++]=0;size_t shstr_name=n-strings;memcpy(b+n,".shstrtab",10);n+=10;
 for(unsigned j=0;j<DEBUG_MAX;j++){name_at[j]=n-strings;size_t z=strlen(names[j]);memcpy(b+n,names[j],z);n+=z;z=strlen(suffix);memcpy(b+n,suffix,z);n+=z;b[n++]=0;}
 size_t string_size=n-strings;
 for(unsigned j=0;j<DEBUG_MAX;j++){data_at[j]=n;if(s->size[j])memcpy(b+n,s->data[j],s->size[j]);n+=s->size[j];}
 memcpy(b,"\177ELF",4);b[4]=2;b[5]=1;b[6]=1;putle(b,16,1,2);putle(b,18,62,2);putle(b,20,1,4);putle(b,40,64,8);putle(b,52,64,2);putle(b,58,64,2);putle(b,60,DEBUG_MAX+2,2);putle(b,62,DEBUG_MAX+1,2);
 for(unsigned j=0;j<DEBUG_MAX;j++){size_t h=64+(j+1)*64;putle(b,h,name_at[j],4);putle(b,h+4,1,4);putle(b,h+24,data_at[j],8);putle(b,h+32,s->size[j],8);putle(b,h+48,1,8);}
 size_t h=64+(DEBUG_MAX+1)*64;putle(b,h,shstr_name,4);putle(b,h+4,3,4);putle(b,h+24,strings,8);putle(b,h+32,string_size,8);putle(b,h+48,1,8);
 FILE *f=fopen(path,"wb");if(!f)return 0;int ok=fwrite(b,1,n,f)==n;return fclose(f)==0&&ok;
}
#endif
