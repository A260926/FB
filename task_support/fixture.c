#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  FILE *f = fopen(argv[1], "wb");
  if (!f) return 3;
  for (int n=0;n<9;n++) {
    for (int y=0;y<64;y++) for (int x=0;x<64;x++)
      if (fputc((x*3+y*2+n*17+(x>n*3&&x<n*3+12?40:0))&255,f)==EOF) return 4;
    for (int p=0;p<2;p++) for (int i=0;i<32*32;i++)
      if (fputc(96+p*40+n*2,f)==EOF) return 4;
  }
  return fclose(f)!=0;
}
