#include <stdio.h>
#include <math.h>
int main(void){FILE *f=fopen(".task-build/loop-region.f32","rb");if(!f)return 1;float x;size_t n=0;double e=0;while(fread(&x,sizeof(x),1,f)==1){if(!isfinite(x))return 2;e+=fabs(x);n++;}long size=ftell(f);fclose(f);if(n<128||n%2||size!=(long)(n*sizeof(float))||e==0)return 3;printf("LOOP_EXAMPLE_FRESH frames=%zu nonzero=1\n",n/2);return 0;}
