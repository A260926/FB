#ifndef TASK_TRACE_H
#define TASK_TRACE_H
#include <stddef.h>
void mikk_edit_trace_face(int original_face);
void task_trace_reset(void);
size_t task_trace_size(void);
int task_trace_at(size_t index);
#endif
