#include "mimalloc.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
static int touch(int before){pid_t c=fork();if(c==0){volatile unsigned char*p=mi_malloc(64);if(!p)_exit(20);size_t n=mi_usable_size((void*)p);if(before)p[-1]=7;else p[n]=7;_exit(0);}int s=0;if(waitpid(c,&s,0)!=c)return -1;return WIFSIGNALED(s)?WTERMSIG(s):0;}
int main(void){mi_theap_t*t=mi_theap_get_default();mi_theap_guarded_set_sample_rate(t,1,1);mi_theap_guarded_set_size_bound(t,1,65536);unsigned char*p=mi_zalloc(64);if(!p)return 1;for(int i=0;i<64;i++)if(p[i])return 2;memset(p,0x34,64);p=mi_realloc(p,900);if(!p)return 3;for(int i=0;i<64;i++)if(p[i]!=0x34)return 4;mi_free(p);int a=touch(1),b=touch(0);if(a||!(b==SIGSEGV||b==SIGBUS))return 5;printf("MIMALLOC realloc=preserved prefix=accessible tail=guarded\n");return 0;}
