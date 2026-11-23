#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static SAMPLE *mk(int kind, int len, int looplen) {
    short *buf=malloc(len*2); int i;
    unsigned char *raw=malloc(len*2);
    for(i=0;i<len;i++){
        short v;
        if(kind==0) v=(short)(((i%17)-8)*1500);
        else if(kind==1) v=(short)((-i*90+3000));
        else v=(short)(((i%23)-11)*900);
        buf[i]=v; raw[2*i]=(unsigned char)(v&255); raw[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
    }
    SAMPLE *s=Sample_LoadRawMem((char*)raw,len*2,44100,1,SF_16BITS|SF_SIGNED);
    free(buf);free(raw);
    s->flags|=SF_LOOP; s->loopstart=0; s->loopend=looplen;
    return s;
}

static int runmode(UWORD mode, int frames, int nframes, short *out, short*out2) {
    static char abuf[4096], bbuf[4096];
    SAMPLE *a,*b; int i;
    MikMod_RegisterDriver(&drv_nos);
    md_mode=mode; md_mixfreq=44100; md_device=1; md_reverb=0;
    md_volume=md_sndfxvolume=128; md_pansep=128;
    if(MikMod_Init("")) return 10;
    if(MikMod_SetNumVoices(0,1)) return 11;
    for(i=0;i<512;i++){short x=(short)(((i%32)-16)*400), y=-8000;
        abuf[2*i]=(char)(x&255);abuf[2*i+1]=(char)(((unsigned short)x)>>8);
        bbuf[2*i]=(char)(y&255);bbuf[2*i+1]=(char)(((unsigned short)y)>>8);}
    a=Sample_LoadRawMem(abuf,sizeof(abuf),44100,1,SF_16BITS|SF_SIGNED);
    b=Sample_LoadRawMem(bbuf,sizeof(bbuf),44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP; a->loopstart=0; a->loopend=a->length;
    b->flags|=SF_LOOP; b->loopstart=0; b->loopend=b->length;
    if(MikMod_EnableOutput())return 13;
    Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,44100);
    Voice_Play(0,a,0);
    VC_WriteBytes((SBYTE*)out,(ULONG)nframes*4);
    Voice_Play(0,b,0);
    VC_WriteBytes((SBYTE*)out2,(ULONG)nframes*4);
    Voice_Stop(0);MikMod_DisableOutput();
    Sample_Free(a);Sample_Free(b);MikMod_Exit();
    return 0;
}

int main(void){
    short old[64*2],incoming[64*2],act[64*2];
    int rc;
    /* fade 0 */
    MikMod_SetRetriggerFade(0);
    rc=runmode(DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP,64,64,old,incoming);
    printf("rc=%d old0=%d in0=%d\n",rc,old[0],incoming[0]);
    MikMod_SetRetriggerFade(32);
    rc=runmode(DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP,64,64,old,act);
    printf("rc=%d actual0=%d\n",rc,act[0]);
    return 0;
}
