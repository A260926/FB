#include "zlib.h"
#include <string.h>
#include <stdio.h>
int main(void){unsigned char in[]="normal stream copy baseline",out[256],back[256];uLongf n=256,m=256;if(compress(out,&n,in,sizeof in)||uncompress(back,&m,out,n)||m!=sizeof in||memcmp(in,back,m))return 1;z_stream a={0},b={0};if(deflateInit(&a,6)||deflateCopy(&b,&a))return 2;deflateEnd(&a);deflateEnd(&b);puts("BASELINE_OK roundtrip_and_copy");return 0;}
