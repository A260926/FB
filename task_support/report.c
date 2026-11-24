/* Only the generic assertion driver's reporting bypasses DeepState's printf interposition. */
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
static int report(int fd,const char *fmt,va_list ap){char b[4096];int n=vsnprintf(b,sizeof(b),fmt,ap);if(n<0 || (size_t)n>=sizeof(b))return -1;size_t off=0;while(off<(size_t)n){ssize_t k=write(fd,b+off,(size_t)n-off);if(k<=0)return -1;off+=(size_t)k;}return n;}
int task_report_printf(const char *fmt,...){va_list ap;va_start(ap,fmt);int r=report(1,fmt,ap);va_end(ap);return r;}
int task_report_fprintf(FILE *f,const char *fmt,...){va_list ap;va_start(ap,fmt);int r=report(f==stderr?2:1,fmt,ap);va_end(ap);return r;}
