#ifndef TASK_EVAL_TRACE_H
#define TASK_EVAL_TRACE_H
#include "sisl.h"
typedef void (*task_eval_observer)(const char *,const void *,int,int,const double *,void *);
void task_eval_set_observer(task_eval_observer,void *);
void task_reference_s1365(SISLSurf *,double,double,double,int,SISLSurf **,int *);
#endif
