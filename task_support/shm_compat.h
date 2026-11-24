/* Environment adapter only: retain production trace_bits, execution and classification.
   Replace SysV allocation with a shared file for restricted local hosts. */
#ifndef TASK_SHM_COMPAT_H
#define TASK_SHM_COMPAT_H
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
static char task_map_path[128];
static size_t task_map_size;
static int task_shmget(key_t key,size_t size,int flags){
 (void)key;(void)flags;
 const char *dir=getenv("TMPDIR");if(!dir||!*dir)dir="/tmp";
 snprintf(task_map_path,sizeof(task_map_path),"%s/afl-task-map-XXXXXX",dir);
 int fd=mkstemp(task_map_path);if(fd<0)return -1;
 if(ftruncate(fd,(off_t)size)||setenv("TASK_COVER_MAP",task_map_path,1)){close(fd);unlink(task_map_path);return -1;}
 task_map_size=size;return fd;
}
static void *task_shmat(int fd,const void *addr,int flags){(void)addr;(void)flags;return mmap(NULL,task_map_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);}
static int task_shmctl(int fd,int cmd,struct shmid_ds *buf){(void)cmd;(void)buf;int r=close(fd);unlink(task_map_path);return r;}
#define shmget task_shmget
#define shmat task_shmat
#define shmctl task_shmctl
#endif
