// Only finite normal text; this fixture never signals or exercises a fault.
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
int main(int argc,char**argv){
 const char *path=getenv("TASK_COVER_MAP");if(!path)return 2;
 int fd=open(path,O_RDWR);if(fd<0)return 3;
 unsigned char *m=mmap(0,MAP_SIZE,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);if(m==MAP_FAILED)return 4;
 int input=0;if(argc==2){input=open(argv[1],O_RDONLY);if(input<0)return 5;}
 unsigned char buf[512];ssize_t n;int a=0,b=0;
 while((n=read(input,buf,sizeof(buf)))>0)for(ssize_t i=0;i<n;i++){a|=buf[i]=='A';b|=buf[i]=='B';}
 if(input)close(input);if(n<0)return 6;
 m[1]=1;if(a)m[2]=1;if(b)m[3]=1;
 munmap(m,MAP_SIZE);return 0;
}
