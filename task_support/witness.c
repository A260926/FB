#include "adapter.h"
#include <string.h>
int main(void){
 setup();zzuf_set_auto_increment();char path[]=".baseline/inputXXXXXX";if(fixture(path))return 2;
 int a=zz_test_open(path,O_RDONLY);unsigned char head[16],want[64],got[64];
 if(zz_test_read(a,head,16)!=16)return 3;
 int b=zz_test_dup(a);if(b<0)return 4;
 long long tracked=(long long)_zz_getpos(b),kernel=(long long)lseek(b,0,SEEK_CUR);
 unsigned s1=_zz_getfuzz(a)->seed,s2=_zz_getfuzz(b)->seed;
 if(zz_test_pread(a,want,64,16)!=64 || zz_test_read(b,got,64)!=64)return 5;
 int diff=0;for(int i=0;i<64;i++)diff+=want[i]!=got[i];
 printf("DUP_WITNESS tracked=%lld kernel=%lld seeds=%u/%u byte_mismatches=%d\n",tracked,kernel,s1,s2,diff);
 zz_test_close(a);zz_test_close(b);unlink(path);
 return tracked==kernel && s1==s2 && diff==0 ? 0:1;
}
