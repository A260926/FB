#include "frei0r.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void image(uint32_t *p, int x) {
 unsigned char *b=(unsigned char*)p;
 for(int i=0;i<2;i++) { b[4*i]=b[4*i+1]=b[4*i+2]=x+100*i; b[4*i+3]=37+i; }
}
int main(void) {
 assert(f0r_init()); f0r_instance_t s=f0r_construct(2,1); assert(s);
 uint32_t in[2],out[2]; double n=2.1/127.0; f0r_set_param_value(s,&n,2);
 image(in,10); f0r_update(s,0,in,out); assert(((unsigned char*)out)[0]==0);
 image(in,30); f0r_update(s,1,in,out); assert(((unsigned char*)out)[0]==26);
 image(in,50); f0r_update(s,2,in,out); assert(((unsigned char*)out)[0]==51);
 n=0; f0r_set_param_value(s,&n,2); image(in,70); f0r_update(s,3,in,out);
 unsigned char *b=(unsigned char*)out;
 printf("shrink N=3->1 observed RGB=%u/%u, desired=0/255\n",b[0],b[4]);
 assert(b[0]!=0 || b[4]!=255); assert(b[3]==37 && b[7]==38);
 f0r_destruct(s); f0r_deinit(); puts("normaliz0r animated-window defect: reproduced");
}
