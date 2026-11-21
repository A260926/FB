#include "eval_trace.h"
static task_eval_observer observer;static void *context;
void task_eval_set_observer(task_eval_observer f,void *p){observer=f;context=p;}
void task_real_s1221(SISLCurve*,int,double,int*,double*,int*);
void task_real_s1227(SISLCurve*,int,double,int*,double*,int*);
void task_real_s1424(SISLSurf*,int,int,double*,int*,int*,double*,int*);
void s1221(SISLCurve*p,int n,double t,int*i,double*d,int*s){if(observer)observer("s1221",p,n,0,&t,context);task_real_s1221(p,n,t,i,d,s);}
void s1227(SISLCurve*p,int n,double t,int*i,double*d,int*s){if(observer)observer("s1227",p,n,0,&t,context);task_real_s1227(p,n,t,i,d,s);}
void s1424(SISLSurf*p,int n,int m,double*t,int*i,int*j,double*d,int*s){if(observer)observer("s1424",p,n,m,t,context);task_real_s1424(p,n,m,t,i,j,d,s);}
