#include "xls.h"
#include <assert.h>
#include <stdio.h>
int main(void){xls_error_t e; xlsWorkBook*w=xls_open_file("test/files/test2.xls","UTF-8",&e);assert(w&&e==LIBXLS_OK);printf("sheets=%u\n",w->sheets.count);xlsWorkSheet*s=xls_getWorkSheet(w,0);assert(s);assert(xls_parseWorkSheet(s)==LIBXLS_OK);printf("rows=%u cols=%u\n",s->rows.lastrow+1,s->rows.lastcol+1);xlsCell*c=xls_cell(s,0,0);assert(c);printf("cell0 id=%x string=%s\n",c->id,c->str?c->str:"");xls_close_WS(s);xls_close_WB(w);puts("LIBXLS_BASELINE_WORKBOOK_AND_CELLS_OK");}
