/* A link consumer and minimal normal-call check; contains no AVI writer. */
#include "smk_export.h"
#include "smk_avi.h"
#include <stdio.h>
#include <string.h>
#include "fixture.h"
int main(int argc,char **argv){FILE *f;unsigned char h[12];if(argc!=3)return 2;
 if(smk_fixture_write(argv[1]) || smk_export_avi(argv[1],argv[2],5))return 3;
 f=fopen(argv[2],"rb");if(!f)return 4;
 if(fread(h,1,12,f)!=12){fclose(f);return 5;}if(fclose(f))return 6;
 return memcmp(h,"RIFF",4)||memcmp(h+8,"AVI ",4)?7:0;
}
