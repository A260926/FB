#include <stdio.h>
int main(int n,char**v){FILE*f;unsigned long bytes=0;int ch,nonzero=0;if(n!=2)return 2;f=fopen(v[1],"rb");if(!f)return 3;while((ch=fgetc(f))!=EOF){bytes++;nonzero|=ch;}if(ferror(f)){fclose(f);return 4;}fclose(f);if(bytes<1024 || bytes%4 || !nonzero)return 5;puts("SMF2_EXAMPLE_FRESH_NONZERO");return 0;}
