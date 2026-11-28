#include "trace.h"
static int faces[1048576];
static size_t used;
void mikk_edit_trace_face(int f){if(used<sizeof(faces)/sizeof(faces[0]))faces[used]=f;used++;}
void task_trace_reset(void){used=0;}
size_t task_trace_size(void){return used;}
int task_trace_at(size_t n){return n<used&&n<sizeof(faces)/sizeof(faces[0])?faces[n]:-1;}
