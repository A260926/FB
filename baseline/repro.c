#include "anim.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#ifdef ANIM_THREAD_SAFE
#include <pthread.h>
#endif
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"probe failed line=%d\n",__LINE__);exit(1);}}while(0)
static int near(float a,float b){return fabsf(a-b)<1e-5f;}
static void init(struct anm_node *n){REQUIRE(anm_init_node(n)==0);REQUIRE(anm_add_node_animation(n)==0);REQUIRE(anm_use_node_animation(n,1)==0);REQUIRE(anm_use_node_animation(n,0)==0);}
#ifdef ANIM_THREAD_SAFE
struct state{struct anm_node *node;pthread_mutex_t lock;pthread_cond_t cond;int phase;float before,after,after_inv;};
static void *worker(void *arg){
 struct state *s=arg;float m[16],inv[16];
 anm_get_matrix(s->node,m,0);anm_get_inv_matrix(s->node,inv,0);s->before=m[12];
 REQUIRE(!pthread_mutex_lock(&s->lock));s->phase=1;REQUIRE(!pthread_cond_broadcast(&s->cond));
 while(s->phase!=2)REQUIRE(!pthread_cond_wait(&s->cond,&s->lock));
 REQUIRE(!pthread_mutex_unlock(&s->lock));
 anm_get_matrix(s->node,m,0);anm_get_inv_matrix(s->node,inv,0);s->after=m[12];s->after_inv=inv[12];return 0;
}
#endif
int main(void){
 struct anm_node root,child;struct anm_animation *a;float m[16],inv[16],fresh[16];
 init(&root);init(&child);anm_link_node(&root,&child);
 anm_set_position3f(&root,0,0,0,0);anm_set_position3f(&root,10,0,0,1000);anm_set_position3f(&child,0,2,0,0);
 anm_get_matrix(&child,m,500);anm_get_inv_matrix(&child,inv,500);REQUIRE(near(m[12],5));
 a=anm_get_animation(&root,0);REQUIRE(anm_set_value(&a->tracks[ANM_TRACK_POS_X],0,20)==0);
 anm_get_node_matrix(&root,fresh,500);anm_get_matrix(&child,m,500);anm_get_inv_matrix(&child,inv,500);
 REQUIRE(near(fresh[12],15)&&near(m[12],5)&&near(inv[12],-5));
 printf("LIBANIM_REPRO_LOWLEVEL local=%.0f cached_world=%.0f cached_inverse=%.0f\n",fresh[12],m[12],inv[12]);
 anm_destroy_node(&child);anm_destroy_node(&root);
#ifdef ANIM_THREAD_SAFE
 {struct state s;pthread_t t;
 init(&root);init(&child);anm_link_node(&root,&child);anm_set_position3f(&root,1,0,0,0);anm_set_position3f(&child,2,0,0,0);
 s.node=&child;s.phase=0;REQUIRE(!pthread_mutex_init(&s.lock,0));REQUIRE(!pthread_cond_init(&s.cond,0));
 REQUIRE(!pthread_create(&t,0,worker,&s));REQUIRE(!pthread_mutex_lock(&s.lock));
 while(s.phase!=1)REQUIRE(!pthread_cond_wait(&s.cond,&s.lock));
 anm_set_position3f(&root,8,0,0,0);anm_get_matrix(&child,m,0);REQUIRE(near(m[12],10));
 s.phase=2;REQUIRE(!pthread_cond_broadcast(&s.cond));REQUIRE(!pthread_mutex_unlock(&s.lock));REQUIRE(!pthread_join(t,0));
 REQUIRE(near(s.before,3)&&near(s.after,3)&&near(s.after_inv,-3));
 printf("LIBANIM_REPRO_THREAD main=%.0f worker=%.0f worker_inverse=%.0f\n",m[12],s.after,s.after_inv);
 REQUIRE(!pthread_cond_destroy(&s.cond));REQUIRE(!pthread_mutex_destroy(&s.lock));anm_destroy_node(&child);anm_destroy_node(&root);
 }
#endif
 return 0;
}
