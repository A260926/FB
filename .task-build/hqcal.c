#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct{const short*d;long long size,reppos,repend,pos,inc;unsigned flags;int live;}RC;
#define FB 28
#define FM 268435455LL
static void rinit(RC*r,const short*d,int n,unsigned fl,int ls,int le,long long pos,long long inc){memset(r,0,sizeof*r);r->d=d;r->size=n;r->flags=fl;r->reppos=ls;r->repend=le;r->pos=pos;r->inc=inc;r->live=1;}
static int rstep(RC*r){long long idxsize=r->size?(r->size<<FB)-1:0,idxlend=r->repend?(r->repend<<FB)-1:0,idxlpos=r->reppos<<FB,end,done,i,f;int s;if(!r->live)return 0;
 if(r->flags&SF_REVERSE){if((r->flags&SF_LOOP)&&(r->pos<idxlpos)){if(r->flags&SF_BIDI){r->pos=idxlpos+(idxlpos-r->pos);r->flags&=~SF_REVERSE;r->inc=-r->inc;}else r->pos=idxlend-(idxlpos-r->pos);}else if(!(r->flags&SF_LOOP)&&r->pos<0){r->live=0;return 0;}}
 else{if((r->flags&SF_LOOP)&&(r->pos>=idxlend)){if(r->flags&SF_BIDI){r->flags|=SF_REVERSE;r->inc=-r->inc;r->pos=idxlend-(r->pos-idxlend);}else r->pos=idxlpos+(r->pos-idxlend);}else if(!(r->flags&SF_LOOP)&&r->pos>=idxsize){r->live=0;return 0;}}
 end=(r->flags&SF_REVERSE)?((r->flags&SF_LOOP)?idxlpos:0):((r->flags&SF_LOOP)?idxlend:idxsize);
 if((r->inc>0&&r->pos>=end)||(r->inc<0&&r->pos<=end)||!r->inc)done=0;else done=(end-r->pos)/r->inc+1;
 if(done<=0){r->live=0;return 0;}
 i=r->pos>>FB;f=r->pos&FM;s=(int)(((long long)r->d[i]*(FM+1-f)+(long long)r->d[i+1]*f)>>FB);r->pos+=r->inc;return s;}
static void gains(int vol,int pan,int stereo,int*l,int*r){if(stereo){if(pan==PAN_SURROUND){*l=*r=(vol*256)/480;}else{*l=(vol*(PAN_RIGHT-pan))>>8;*r=(vol*pan)>>8;}}else{*l=vol;*r=0;}}
static void acc(RC*r,int lv,int rv,int st,int su,long long*L,long long*R){int s=rstep(r);*L=*R=0;if(st){if(su){long long v;if(lv>=rv){v=(long long)lv*s;*L=v;*R=-v;}else{v=(long long)rv*s;*L=-v;*R=v;}}else{*L=(long long)lv*s;*R=(long long)rv*s;}}else *L=(long long)lv*s;}
static long long cl(long long v,long long lo,long long hi){return v<lo?lo:v>hi?hi:v;}

static short rout[32768]; static float fout[32768];
static void run(int stage,int stereo,int surr,int pan,int frq,int nfade,int N,int nout){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade(nfade);
  UWORD m=DMODE_HQMIXER|DMODE_SOFT_SNDFX|DMODE_INTERP; if(stereo)m|=DMODE_STEREO; if(surr)m|=DMODE_SURROUND;
  if(stage==0)m|=DMODE_16BITS; if(stage==2)m|=DMODE_FLOAT;
  md_mode=m;md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char ra[2048],rb[2048];int i;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);ra[2*i]=(unsigned char)(v&255);ra[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
    short y=(short)(500-i*3);rb[2*i]=(unsigned char)(y&255);rb[2*i+1]=(unsigned char)(((unsigned short)y)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)ra,1024,44100,1,SF_16BITS|SF_SIGNED);SAMPLE*b=Sample_LoadRawMem((char*)rb,1024,44100,1,SF_16BITS|SF_SIGNED);
  a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;b->flags|=SF_LOOP;b->loopstart=0;b->loopend=512;
  MikMod_EnableOutput();Voice_SetVolume(0,256);Voice_SetPanning(0,pan);Voice_SetFrequency(0,frq);
  Voice_Play(0,a,0);
  {static short w[16384];VC_WriteBytes((SBYTE*)w,(ULONG)(64*(stage==2?4:stage==0?2:1)*(stereo?2:1)));}
  Voice_Play(0,b,0);
  {int bytes=nout*(stage==2?4:stage==0?2:1)*(stereo?2:1);if(stage==2)VC_WriteBytes((SBYTE*)fout,bytes);else VC_WriteBytes((SBYTE*)rout,bytes);}
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
int main(void){
  int stage,stereo,surr,pan,frq,N;
  for(stage=0;stage<3;stage++)for(stereo=0;stereo<2;stereo++)for(surr=0;surr<2;surr++)for(pan=0;pan<4;pan++){
    static const int pans[4]={128,200,60,512}; if(!stereo&&surr&&pan==3)continue;
    int maxd=0;
    for(frq=22050;frq<=44100;frq+=11025)for(N=1;N<=16;N*=16){
      short apad[600],bpad[600];int i,k;
      for(i=0;i<512;i++){apad[i]=(short)(((i%17)-8)*1500);bpad[i]=(short)(500-i*3);}for(i=0;i<20;i++){apad[512+i]=0;bpad[512+i]=0;}
      long long inc=((long long)frq<<(28-2))/44100;
      RC o,n; int ol,orr,nl,nr;int es=surr&&pans[pan]==PAN_SURROUND;
      rinit(&o,apad,512,SF_LOOP,0,512,64LL*4*inc,inc);rinit(&n,bpad,512,SF_LOOP,0,512,0,inc);
      gains(256,pans[pan],stereo,&ol,&orr);gains(256,pans[pan],stereo,&nl,&nr);
      run(stage,stereo,es,pans[pan],frq,N,32,32);
      for(i=0;i<32;i++){
        long long sl=0,sr=0,cL[4],cR[4]; int kk;
        long long wj=i<N?i:N,wo=i<N?N-i:0,den=N;
        for(kk=0;kk<4;kk++){long long aL,aR,bL,bR;acc(&o,ol,orr,stereo,es,&aL,&aR);acc(&n,nl,nr,stereo,es,&bL,&bR);cL[kk]=(wo*aL+wj*bL)/den;cR[kk]=(wo*aR+wj*bR)/den;}
        if(stage==0){long long s1=0,s2=0;for(kk=0;kk<4;kk++){s1+=cl(cL[kk]/512,-32768,32767);s2+=cl(cR[kk]/512,-32768,32767);}
          {int gl,gr;gl=stereo?rout[i*2]:rout[i];gr=stereo?rout[i*2+1]:0;int ml=(int)(s1/4),mr=(int)(s2/4);int d1=abs(gl-ml),d2=stereo?abs(gr-mr):0;if(d1>maxd)maxd=d1;if(d2>maxd)maxd=d2;}}
        else if(stage==1){long long s1=0,s2=0;for(kk=0;kk<4;kk++){s1+=cl(cL[kk]/131072,-128,127);s2+=cl(cR[kk]/131072,-128,127);}
          {int gl,gr;gl=(unsigned char)((SBYTE*)rout)[i*(stereo?2:1)];gr=stereo?(unsigned char)((SBYTE*)rout)[i*(stereo?2:1)+1]:0;int ml=(int)(s1/4)+128,mr=(int)(s2/4)+128;int d1=abs(gl-ml),d2=stereo?abs(gr-mr):0;if(d1>maxd)maxd=d1;if(d2>maxd)maxd=d2;}}
        else {double s1=0,s2=0;for(kk=0;kk<4;kk++){double x1=(double)cL[kk]/16777216.0,x2=(double)cR[kk]/16777216.0;if(x1>1)x1=1;if(x1<-1)x1=-1;if(x2>1)x2=1;if(x2<-1)x2=-1;s1+=x1;s2+=x2;}
          {double gl=fout[i*(stereo?2:1)],gr=stereo?fout[i*(stereo?2:1)+1]:0;double ml=s1/4,mr=s2/4;if(fabs(gl-ml)>1e-5||fabs(gr-mr)>1e-5){int d=(int)(fabs(gl-ml)*100000);if(d>maxd)maxd=d;}}}
      }
    }
    if(maxd)printf("stage=%d st=%d surr=%d pan=%d MAXDIFF=%d\n",stage,stereo,surr,pans[pan],maxd);
  }
  printf("done\n");
  return 0;
}
