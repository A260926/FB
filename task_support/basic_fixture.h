#ifndef BASIC_FIXTURE_H
#define BASIC_FIXTURE_H
#include <stdint.h>
#include <string.h>
#include "backtrace.h"
#include "internal.h"
/* Legal synthetic section bytes. The builder only encodes ordinary source/PC
   mappings and a skeleton. It contains no split-unit resolver. */
struct fxbuf { unsigned char b[2048]; size_t n; };
struct basic_fixture { struct fxbuf info, abbrev, line; struct dwarf_sections sections; };
static void fx8(struct fxbuf *b, unsigned v) { b->b[b->n++]=(unsigned char)v; }
static void fxn(struct fxbuf *b, uint64_t v, unsigned n) { while(n--){fx8(b,(unsigned)v);v>>=8;} }
static void fxs(struct fxbuf *b, const char *s) { do {fx8(b,(unsigned char)*s);} while(*s++); }
static void fxlen(struct fxbuf *b, size_t at) { unsigned v=(unsigned)(b->n-at-4); for(unsigned i=0;i<4;i++) b->b[at+i]=(unsigned char)(v>>(i*8)); }
static void make_basic_fixture(struct basic_fixture *f, int skeleton) {
 memset(f,0,sizeof(*f));
 struct fxbuf *a=&f->abbrev,*i=&f->info,*l=&f->line;
 /* code 1, compile/skeleton, children; name:string, low_pc:addr,
    high_pc:data4, stmt_list:sec_offset. */
 fx8(a,1); fx8(a,skeleton?0x4a:0x11); fx8(a,!skeleton);
 fx8(a,3);fx8(a,8);fx8(a,0x11);fx8(a,1);fx8(a,0x12);fx8(a,6);fx8(a,0x10);fx8(a,0x17);fx8(a,0);fx8(a,0);
 if(!skeleton){fx8(a,2);fx8(a,0x2e);fx8(a,0);fx8(a,3);fx8(a,8);fx8(a,0x11);fx8(a,1);fx8(a,0x12);fx8(a,6);fx8(a,0);fx8(a,0);} fx8(a,0);
 fxn(i,0,4);fxn(i,5,2);fx8(i,skeleton?4:1);fx8(i,8);fxn(i,0,4);
 if(skeleton)fxn(i,UINT64_C(0x1122334455667788),8);
 fx8(i,1);fxs(i,"sample.c");fxn(i,0x1000,8);fxn(i,0x20,4);fxn(i,0,4);
 if(!skeleton){fx8(i,2);fxs(i,"ordinary");fxn(i,0x1000,8);fxn(i,0x20,4);fx8(i,0);} fxlen(i,0);
 /* DWARF5 line header: one directory, file index 0; program set_file 0. */
 fxn(l,0,4);fxn(l,5,2);fx8(l,8);fx8(l,0);size_t hl=l->n;fxn(l,0,4);size_t hs=l->n;
 fx8(l,1);fx8(l,1);fx8(l,1);fx8(l,251);fx8(l,14);fx8(l,13);
 const unsigned char lens[]={0,1,1,1,1,0,0,0,1,0,0,1};for(unsigned j=0;j<12;j++)fx8(l,lens[j]);
 fx8(l,1);fx8(l,1);fx8(l,8);fx8(l,1);fxs(l,"/synthetic");
 fx8(l,2);fx8(l,1);fx8(l,8);fx8(l,2);fx8(l,0x0b);fx8(l,1);fxs(l,"sample.c");fx8(l,0);
 unsigned h=(unsigned)(l->n-hs);for(unsigned j=0;j<4;j++)l->b[hl+j]=(unsigned char)(h>>(8*j));
 fx8(l,0);fx8(l,9);fx8(l,2);fxn(l,0x1000,8);fx8(l,4);fx8(l,0);fx8(l,3);fx8(l,6);fx8(l,1);
 fx8(l,2);fx8(l,0x20);fx8(l,0);fx8(l,1);fx8(l,1);fxlen(l,0);
 f->sections.data[DEBUG_INFO]=i->b;f->sections.size[DEBUG_INFO]=i->n;
 f->sections.data[DEBUG_ABBREV]=a->b;f->sections.size[DEBUG_ABBREV]=a->n;
 f->sections.data[DEBUG_LINE]=l->b;f->sections.size[DEBUG_LINE]=l->n;
}
#endif
