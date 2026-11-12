#include "vterm.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){VTerm*v=vterm_new(8,6);vterm_set_utf8(v,1);VTermScreen*s=vterm_obtain_screen(v);vterm_screen_reset(s,1);vterm_screen_enable_reflow(s,1);
const char*t="ab\xe4\xb8\xad""cd";vterm_input_write(v,t,strlen(t));VTermScreenCell cell;vterm_screen_get_cell(s,(VTermPos){0,2},&cell);assert(cell.chars[0]==0x4e2d&&cell.width==2);
vterm_set_size(v,8,3);for(int r=0;r<2;r++)for(int c=0;c<3;c++){vterm_screen_get_cell(s,(VTermPos){r,c},&cell);printf("REFLOW_OBS row=%d col=%d cp=%x width=%d\n",r,c,cell.chars[0],cell.width);}
vterm_free(v);v=vterm_new(6,6);s=vterm_obtain_screen(v);vterm_screen_reset(s,1);vterm_screen_enable_reflow(s,1);vterm_input_write(v,"abcde",5);vterm_set_size(v,6,3);vterm_screen_get_cell(s,(VTermPos){1,0},&cell);assert(cell.chars[0]=='d');vterm_free(v);puts("VTERM_BASELINE_WIDE_SOURCE_ASCII_REFLOW_OK");}
