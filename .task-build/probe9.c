#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static short out[8192];
static void run(int hq,int nfade,int nread,int chunk,unsigned aflags,int aloop,unsigned bflags,int bloop){
    MikMod_RegisterDriver(&drv_nos);
    MikMod_SetRetriggerFade((ULONG)nfade);
    md_mode=(hq?DMODE_HQMIXER:0)|DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    unsigned char rawa[1024],rawb[1024];
    for(int i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);rawa[2*i]=(unsigned char)(v&255);rawa[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
        short y=(short)(500-i*3);rawb[2*i]=(unsigned char)(y&255);rawb[2*i+1]=(unsigned char)(((unsigned short)y)>>8);}
    SAMPLE*a=Sample_LoadRawMem((char*)rawa,1024,44100,1,SF_16BITS|SF_SIGNED);
    SAMPLE*b=Sample_LoadRawMem((char*)rawb,1024,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=aflags; if(aflags&SF_LOOP){a->loopstart=aloop;a->loopend=a->length;}
    b->flags|=bflags; if(bflags&SF_LOOP){b->loopstart=bloop;b->loopend=b->length;}
    MikMod_EnableOutput();
    Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
    Voice_Play(0,a,0);
    { short warm[512]; VC_WriteBytes((SBYTE*)warm,sizeof(warm)); }
    Voice_Play(0,b,0);
    int n=0;
    while(n<nread){int c=chunk;if(n+c>nread)c=nread-n;VC_WriteBytes((SBYTE*)(out+2*n),(ULONG)c*4);n+=c;}
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
static int cmpc(int hq,int nf,int fl,int al,unsigned af,unsigned bf,int bl){
    static short A[8192],B[8192],C[8192]; int i,d1=0,d7=0,d31=0;
    run(hq,nf,40,40,af,al,bf,bl); memcpy(A,out,sizeof A);
    run(hq,nf,40,1,af,al,bf,bl); memcpy(B,out,sizeof B);
    run(hq,nf,40,7,af,al,bf,bl); memcpy(C,out,sizeof C);
    for(i=0;i<80;i++){if(A[i]!=B[i])d1++; if(A[i]!=C[i])d31++;}
    printf("hq=%d nf=%d fl=%x al=%d bf=%x bl=%d diff1=%d diff7=%d\n",hq,nf,af,al,bf,bl,d1,d31);
    if(d1||d31){printf(" big:");for(i=0;i<8;i++)printf(" %d",A[i]);printf("\n c1 :");for(i=0;i<8;i++)printf(" %d",B[i]);printf("\n c7 :");for(i=0;i<8;i++)printf(" %d",C[i]);printf("\n");}
    return d1+d31;
}
int main(void){int t=0;
 t+=cmpc(0,8,SF_LOOP,0,0,SF_LOOP,0);
 t+=cmpc(0,8,SF_LOOP,300,0,SF_LOOP,0);
 t+=cmpc(0,8,SF_LOOP|SF_BIDI,300,0,SF_LOOP,0);
 t+=cmpc(0,8,SF_LOOP|SF_REVERSE,0,0,SF_LOOP,0);
 t+=cmpc(1,8,SF_LOOP,0,0,SF_LOOP,0);
 t+=cmpc(1,8,SF_LOOP|SF_BIDI,300,0,SF_LOOP,0);
 printf("TOTAL DIFF %d\n",t);
 return 0;}
