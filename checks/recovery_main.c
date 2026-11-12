#include <stdio.h>
extern int exfat_recovery_test(const char *group);
int main(int argc,char **argv){int n;if(argc!=2)return 2;n=exfat_recovery_test(argv[1]);if(n<=0)return 3;printf("EXFAT_RECOVERY_GROUP %s assertions=%d\n",argv[1],n);puts("EXFAT_RECOVERY_GROUP_COMPLETE");return 0;}
