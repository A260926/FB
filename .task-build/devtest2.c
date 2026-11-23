#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- independent reference model ---- */
typedef struct { int n; short *d; int loopstart, loopend; unsigned flags; long long pos; long long inc; } REF;
/* one internal sample (linear interp with 11 frac bits), with loop rules akin to mixer */
static int ref_sample(REF *r){
    /* index globals as in mixer */
    long long idxsize=((long long)r->n<<11)-1;
    long long idxlend=((long long)r->loopend<<11)-1;
    long long idxlpos=(long long)r->loopstart<<11;
    long long i=r->pos>>11, f=r->pos&2047;
    int v=r->d[i]+(((int)(r->d[i+1]-r->d[i]))*f>>11);
    return v;
}
static void ref_advance(REF *r){
    long long idxsize=((long long)r->n<<11)-1;
    long long idxlend=((long long)r->loopend<<11)-1;
    long long idxlpos=(long long)r->loopstart<<11;
    r->pos+=r->inc;
    if(r->flags&SF_REVERSE){
        if((r->flags&SF_LOOP)&&(r->pos<idxlpos)){
            if(r->flags&SF_BIDI){r->pos=idxlpos+(idxlpos-r->pos);r->flags^=SF_REVERSE;r->inc=-r->inc;}
            else r->pos=idxlend-(idxlpos-r->pos);
        } else if(!(r->flags&SF_LOOP) && r->pos<0){ r->pos=0; }
    } else {
        if((r->flags&SF_LOOP)&&(r->pos>=idxlend)){
            if(r->flags&SF_BIDI){r->flags|=SF_REVERSE;r->inc=-r->inc;r->pos=idxlend-(r->pos-idxlend);}
            else r->pos=idxlpos+(r->pos-idxlend);
        } else if(!(r->flags&SF_LOOP)&& r->pos>=idxsize){ r->pos=0; }
    }
}

int main(void){return 0;}
