#include <stdio.h>
/* CC0: deterministic original RGB pattern, 48x32, 38 frames. */
int main(void) {
 for(int t=0;t<38;t++) for(int y=0;y<32;y++) for(int x=0;x<48;x++) {
  unsigned char p[3]={(unsigned char)((x*5+t*7)%256),(unsigned char)((y*7+t*3)%256),(unsigned char)(((x/8+y/8+t/3)%2)?210:30)};
  if(fwrite(p,1,3,stdout)!=3)return 1;
 }
 return ferror(stdout)?1:0;
}
