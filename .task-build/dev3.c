#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAC 11
#define FMASK 2047

typedef struct {
    int n; short *d; int loopstart,loopend; unsigned flags;
    long long pos; long long inc;
} REF;

/* mixer-equivalent next internal sample + advance, for one internal step */
static int ref_next(REF *r){
    long long idxsize=((long long)r->n<<FRAC)-1;
    long long idxlend=((long long)r->loopend<<FRAC)-1;
    long long idxlpos=(long long)r->loopstart<<FRAC;
    long long i=r->pos>>FRAC; int f=(int)(r->pos&FMASK);
    int s=r->d[i]+(((int)(r->d[i+1]-r->d[i]))*f>>FRAC);
    /* advance */
    r->pos+=r->inc;
    if(r->flags&SF_REVERSE){
        if((r->flags&SF_LOOP)&&(r->pos<idxlpos)){
            if(r->flags&SF_BIDI){r->pos=idxlpos+(idxlpos-r->pos);r->flags&=~SF_REVERSE;r->inc=-r->inc;}
            else r->pos=idxlend-(idxlpos-r->pos);
        }
    } else {
        if((r->flags&SF_LOOP)&&(r->pos>=idxlend)){
            if(r->flags&SF_BIDI){r->flags|=SF_REVERSE;r->inc=-r->inc;r->pos=idxlend-(r->pos-idxlend);}
            else r->pos=idxlpos+(r->pos-idxlend);
        }
    }
    return s;
}

static void fill(short *d,int n,int kind){int i;for(i=0;i<n;i++){int v;
  if(kind==0)v=((i%17)-8)*1500; else if(kind==1)v=-i*90+3000; else v=((i%23)-11)*900;
  d[i]=v; d[n+i]=d[i]; /* pad for interp */ }}

int main(void){
    /* model main(a) then departing(a) and main(b) simultaneously, only for a plain
       forward looping 16-bit stereo case with equal step */
    return 0;
}
