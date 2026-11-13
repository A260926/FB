#include "config.h"
#include "libtar.h"
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>
static unsigned char input[512*7];
static size_t pos;
static ssize_t memread(int fd,void *out,size_t n){(void)fd;if(pos+n>sizeof(input))return 0;memcpy(out,input+pos,n);pos+=n;return n;}
static int memclose(int fd){(void)fd;return 0;}
static void hdr(size_t at,char type,const char *name,size_t n){TAR t;memset(&t,0,sizeof(t));strncpy(t.th_buf.name,name,99);t.th_buf.typeflag=type;th_set_size(&t,(int)n);th_set_mode(&t,0644);th_finish(&t);memcpy(input+at,&t.th_buf,512);}
int main(void){TAR *t; tartype_t io={0}; char name[151];memset(name,'n',150);name[150]=0;
 io.readfunc=memread;io.closefunc=memclose;assert(tar_fdopen(&t,0,"memory",&io,O_RDONLY,0,TAR_GNU)==0);
 hdr(0,'L',"././@LongLink",151);memcpy(input+512,name,151);
 hdr(1024,'0',"short",0);hdr(1536,'0',"next",0);
 assert(th_read(t)==0);assert(strcmp(th_get_pathname(t),name)==0);
 assert(th_read(t)==0);assert(strcmp(th_get_pathname(t),"next")==0);
 assert(th_read(t)==1);assert(tar_close(t)==0);
 puts("LIBTAR_LONGNAME_LIFETIME_BASELINE_OK");return 0;}
