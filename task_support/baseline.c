#include "theft.h"
#include <stdio.h>
static int live,final_value=-1,calls;
static enum theft_alloc_res alloc8(struct theft*t,void*env,void**out){(void)t;(void)env;int*p=malloc(sizeof*p);if(!p)return THEFT_ALLOC_ERROR;*p=8;*out=p;live++;return THEFT_ALLOC_OK;}
static void release(void*p,void*env){(void)env;if(p){live--;free(p);}}
static enum theft_shrink_res shrink1(struct theft*t,const void*p,uint32_t tactic,void*env,void**out){(void)t;(void)env;if(tactic>0||*(const int*)p==0)return THEFT_SHRINK_NO_MORE_TACTICS;int*q=malloc(sizeof*q);if(!q)return THEFT_SHRINK_ERROR;*q=*(const int*)p-1;*out=q;live++;return THEFT_SHRINK_OK;}
static enum theft_trial_res prop(struct theft*t,void*p){(void)t;calls++;return *(int*)p>=3?THEFT_TRIAL_FAIL:THEFT_TRIAL_PASS;}
static enum theft_hook_counterexample_res capture(const struct theft_hook_counterexample_info*i,void*e){(void)e;final_value=*(int*)i->args[0];return THEFT_HOOK_COUNTEREXAMPLE_CONTINUE;}
int main(void){
 struct theft_type_info ti={.alloc=alloc8,.free=release,.shrink=shrink1};
 struct theft_run_config cfg={.prop1=prop,.type_info={&ti},.trials=1,.seed=1644,.hooks={.counterexample=capture}};
 enum theft_run_res r=theft_run(&cfg);
 if(r!=THEFT_RUN_FAIL||final_value!=3||live||calls<7)return 1;
 printf("THEFT_SEED_SHRINK_OK final=%d live=%d calls=%d\n",final_value,live,calls);return 0;
}
