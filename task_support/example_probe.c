#include <stdio.h>
#include <string.h>
/* Read this invocation's output, independently of tests. Detailed RIFF tests belong to the new test file. */
int main(int argc,char **argv){FILE*f;unsigned char h[12];if(argc!=2)return 2;f=fopen(argv[1],"rb");if(!f)return 3;
 if(fread(h,1,12,f)!=12){fclose(f);return 4;}if(fclose(f))return 5;
 if(memcmp(h,"RIFF",4)||memcmp(h+8,"AVI ",4))return 6;puts("EXAMPLE_FRESH_RIFF_PASS");return 0;}
