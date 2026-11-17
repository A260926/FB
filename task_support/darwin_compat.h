/* Build-only Darwin adapter: POSIX mutex/condition counting semaphore.
 * It leaves the codec and the frame queues unchanged. */
#ifndef TASK_DARWIN_COMPAT_H
#define TASK_DARWIN_COMPAT_H
#ifdef __APPLE__
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; unsigned value; } task_sem_t;
static inline int task_sem_init(task_sem_t *s,int shared,unsigned value) {
    int e; (void)shared; s->value=value;
    if((e=pthread_mutex_init(&s->mutex,0))) {errno=e;return -1;}
    if((e=pthread_cond_init(&s->cond,0))) {pthread_mutex_destroy(&s->mutex);errno=e;return -1;}
    return 0;
}
static inline int task_sem_destroy(task_sem_t *s) {
    int a=pthread_cond_destroy(&s->cond),b=pthread_mutex_destroy(&s->mutex);
    if(a||b){errno=a?a:b;return -1;} return 0;
}
static inline int task_sem_post(task_sem_t *s) {
    int e=pthread_mutex_lock(&s->mutex);if(e){errno=e;return -1;}
    ++s->value;pthread_cond_signal(&s->cond);pthread_mutex_unlock(&s->mutex);return 0;
}
static inline int task_sem_wait(task_sem_t *s) {
    int e=pthread_mutex_lock(&s->mutex);if(e){errno=e;return -1;}
    while(!s->value) if((e=pthread_cond_wait(&s->cond,&s->mutex))){pthread_mutex_unlock(&s->mutex);errno=e;return -1;}
    --s->value;pthread_mutex_unlock(&s->mutex);return 0;
}
static inline int task_spin_init(pthread_mutex_t *s,int shared){(void)shared;return pthread_mutex_init(s,0);}
#define sem_t task_sem_t
#define sem_init task_sem_init
#define sem_destroy task_sem_destroy
#define sem_post task_sem_post
#define sem_wait task_sem_wait
#define pthread_spinlock_t pthread_mutex_t
#define pthread_spin_init task_spin_init
#define pthread_spin_destroy pthread_mutex_destroy
#define pthread_spin_lock pthread_mutex_lock
#define pthread_spin_unlock pthread_mutex_unlock
#endif
#endif
