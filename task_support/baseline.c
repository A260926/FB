#include "adapter.h"
#include <string.h>
int main(void){
 setup();char path[]=".baseline/inputXXXXXX";if(fixture(path))return 2;
 int a=zz_test_open(path,O_RDONLY),b=zz_test_open(path,O_RDONLY);
 unsigned char full[4096],split[4096];
 if(zz_test_read(a,full,4096)!=4096)return 3;
 if(zz_test_read(b,split,1009)!=1009 || zz_test_read(b,split+1009,3087)!=3087)return 4;
 int changed=0;for(int i=0;i<4096;i++)changed+=full[i]!=(unsigned char)(i*13+7);
 if(memcmp(full,split,4096)||!changed)return 5;
 printf("POSITIONAL_FUZZ_OK changed=%d split=1009+3087\n",changed);
 zz_test_close(a);zz_test_close(b);unlink(path);return 0;
}
