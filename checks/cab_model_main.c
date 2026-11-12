#include <stdio.h>
#include <string.h>
extern int cab_model_test(const char *group);
int main(int argc,char **argv){int n;if(argc!=2)return 2;n=cab_model_test(argv[1]);if(n<=0)return 3;printf("CAB_MODEL_GROUP %s assertions=%d\n",argv[1],n);puts("CAB_MODEL_GROUP_COMPLETE");return 0;}
