#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include "task_support/elf_fixture.h"
extern int backtrace_initialize(struct backtrace_state *,const char *,int,backtrace_error_callback,void *,fileline *);
static int errors,seen;static const char *want;
static void error(void *d,const char *s,int e){(void)d;(void)e;fprintf(stderr,"DWARF_ERROR %s\n",s);errors++;}
static int row(void *d,uintptr_t pc,const char *file,int line,const char *fun){(void)d;printf("PC=%lx file=%s line=%d function=%s\n",(unsigned long)pc,file?file:"-",line,fun?fun:"-");seen++;if(!file||!strstr(file,"sample.c")||line!=7||(want?(!fun||strcmp(fun,want)):(fun!=NULL)))errors++;return 0;}
int main(void){for(int s=0;s<2;s++){
 struct basic_fixture f;make_basic_fixture(&f,s);struct backtrace_state st;memset(&st,0,sizeof(st));fileline fn=NULL;struct dwarf_data *entry=NULL;struct libbacktrace_base_address base={0};
 if(!backtrace_dwarf_add(&st,base,&f.sections,0,NULL,error,NULL,&fn,&entry)||!fn||!entry)return 2;
 st.fileline_fn=fn;want=s?NULL:"ordinary";seen=0;backtrace_pcinfo(&st,0x1001,row,error,NULL);if(seen!=1||errors)return 3;
 } {
 struct basic_fixture f;make_basic_fixture(&f,0);if(!write_basic_elf(".task-build/basic.elf",&f.sections,""))return 4;
 struct backtrace_state st;memset(&st,0,sizeof(st));int fd=backtrace_open(".task-build/basic.elf",error,NULL,NULL);if(fd<0)return 5;fileline fn=NULL;
 if(!backtrace_initialize(&st,".task-build/basic.elf",fd,error,NULL,&fn)||!fn)return 6;st.fileline_fn=fn;want="ordinary";seen=0;backtrace_pcinfo(&st,0x1001,row,error,NULL);if(seen!=1||errors)return 7;puts("ELF_READER_OK");
 }puts("BASELINE_OK ordinary function and skeleton line-only; split attach API absent");return 0;}
