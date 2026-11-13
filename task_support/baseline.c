#include "ares.h"
#include <string.h>
#include <stdio.h>
int main(void){unsigned char b[]={3,119,119,119,7,101,120,97,109,112,108,101,0,0xc0,0};char* name=0;long len=0;int r=ares_expand_name(b+13,b,sizeof b,&name,&len);if(r||len!=2||strcmp(name,"www.example"))return 1;ares_free_string(name);puts("BASELINE_OK compressed_name=www.example consumed=2");return 0;}
