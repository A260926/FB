#include "fuzz/stateful/model.h"
#include <stdio.h>
#include <string.h>
int main(void){FILE*i=tmpfile(),*o=tmpfile();char b[512];size_t n;int r;if(!i||!o)return 71;
 fputs("INIT 256 4\nSECTION 0 6c696e6b 666f6f\nENCODER 4096\nREQUEST 0 4096\nFEEDBACK 4096\nEND\n",i);rewind(i);r=qpack_model_replay(i,o);rewind(o);n=fread(b,1,sizeof(b)-1,o);b[n]=0;fclose(i);fclose(o);
 if(r||strcmp(b,"COMPLETE 1 CANCELED 0\nCONSISTENT\n"))return 72;return 0;}
