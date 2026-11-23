#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FB 11
#define FBM 2047LL
typedef struct{const short*d;long long size,reppos,repend;unsigned flags;long long pos,inc;int live;} RC;
static void rinit(RC*r,const short*d,int n,unsigned fl,int ls,int le,long long pos,long long inc){memset(r,0,sizeof* r);r->d=d;r->size=n;r->flags=fl;r->reppos=ls;r->repend=le;r->pos=pos;r->inc=inc;r->live=1;}
static long long isz(RC*r){return r->size?(r->size<<FB)-1:0;}
static long long ilend(RC*r){return r->repend?(r->repend<<FB)-1:0;}
static long long ilpos(RC*r){return r->reppos<<FB;}
static int rstep(RC*r,int*alive){long long idxsize=isz(r),idxlend=ilend(r),idxlpos=ilpos(r),end,done,i,f;int s;*alive=0;if(!r->live)return 0;
 if(r->flags&SF_REVERSE){if((r->flags&SF_LOOP)&&(r->pos<idxlpos)){if(r->flags&SF_BIDI){r->pos=idxlpos+(idxlpos-r->pos);r->flags&=~SF_REVERSE;r->inc=-r->inc;}else r->pos=idxlend-(idxlpos-r->pos);}else if(!(r->flags&SF_LOOP)&&r->pos<0){r->live=0;return 0;}}
 else{if((r->flags&SF_LOOP)&&(r->pos>=idxlend)){if(r->flags&SF_BIDI){r->flags|=SF_REVERSE;r->inc=-r->inc;r->pos=idxlend-(r->pos-idxlend);}else r->pos=idxlpos+(r->pos-idxlend);}else if(!(r->flags&SF_LOOP)&&r->pos>=idxsize){r->live=0;return 0;}}
 end=(r->flags&SF_REVERSE)?((r->flags&SF_LOOP)?idxlpos:0):((r->flags&SF_LOOP)?idxlend:idxsize);
 if((r->inc>0&&r->pos>=end)||(r->inc<0&&r->pos<=end)||!r->inc)done=0;else done=(end-r->pos)/r->inc+1;
 if(done<=0){r->live=0;return 0;}
 i=r->pos>>FB;f=r->pos&FBM;s=(int)r->d[i]+(int)(((long long)(r->d[i+1]-r->d[i])*f)>>FB);r->pos+=r->inc;*alive=1;return s;}
static long long sat(long long v){if(v>=32768)return 32767;if(v< -32768)return -32768;return v;}
static int q16(long long a){return (int)sat(a>>9);}         /* VC1 16-bit */
static int q16hq(long long s4){return (int)(s4/2048);}      /* sum of 4 acc / (512*4) */
static int q8(long long a){return (int)(sat(a>>17)+128);}
static int q8hq(long long s4){return (int)((s4/4)/131072+128);} /* HQ: per acc/131072 clamp then avg */

/* one output frame: acc for old and new, per channel */
int main(void){return 0;}
