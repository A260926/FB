#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { const short *d; long long size,reppos,repend,pos,inc; unsigned flags; int fb,hq,live; } RC;
static long long FM(int fb){return (1LL<<fb)-1;}
static void rinit(RC*r,const short*d,int n,unsigned fl,int ls,int le,long long pos,long long inc,int fb,int hq){
  memset(r,0,sizeof *r); r->d=d;r->size=n;r->flags=fl;r->reppos=ls;r->repend=le;r->pos=pos;r->inc=inc;r->fb=fb;r->hq=hq;r->live=1;}
static int rstep(RC*r){
  long long fb=r->fb,M=FM(fb),idxsize=r->size?(r->size<<fb)-1:0,idxlend=r->repend?(r->repend<<fb)-1:0,idxlpos=r->reppos<<fb,end,done,i,f;int s;
  if(!r->live)return 0;
  if(r->flags&SF_REVERSE){
    if((r->flags&SF_LOOP)&&(r->pos<idxlpos)){
      if(r->flags&SF_BIDI){r->pos=idxlpos+(idxlpos-r->pos);r->flags&=~SF_REVERSE;r->inc=-r->inc;}
      else r->pos=idxlend-(idxlpos-r->pos);
    } else if(!(r->flags&SF_LOOP)&&r->pos<0){r->live=0;return 0;}
  } else {
    if((r->flags&SF_LOOP)&&(r->pos>=idxlend)){
      if(r->flags&SF_BIDI){r->flags|=SF_REVERSE;r->inc=-r->inc;r->pos=idxlend-(r->pos-idxlend);}
      else r->pos=idxlpos+(r->pos-idxlend);
    } else if(!(r->flags&SF_LOOP)&&r->pos>=idxsize){r->live=0;return 0;}
  }
  end=(r->flags&SF_REVERSE)?((r->flags&SF_LOOP)?idxlpos:0):((r->flags&SF_LOOP)?idxlend:idxsize);
  if((r->inc>0&&r->pos>=end)||(r->inc<0&&r->pos<=end)||!r->inc)done=0;else done=(end-r->pos)/r->inc+1;
  if(done<=0){r->live=0;return 0;}
  i=r->pos>>fb; f=r->pos&M;
  if(!r->hq) s=r->d[i]+(int)(((long long)(r->d[i+1]-r->d[i])*f)>>fb);
  else s=(int)(((long long)r->d[i]*(M+1-f)+(long long)r->d[i+1]*f)>>fb);
  r->pos+=r->inc; return s;
}
static void gains(int vol,int pan,int stereo,int surround,int hq,int*l,int*r){
  if(stereo){
    if(surround&&pan==PAN_SURROUND){*l=*r=hq?(vol*256)/480:vol/2;}
    else {*l=(vol*(PAN_RIGHT-pan))>>8; *r=(vol*pan)>>8;}
  } else {*l=vol;*r=0;}
}
static void acc(RC*r,int lv,int rv,int stereo,int surround,long long*aL,long long*aR){
  int s=rstep(r); *aL=*aR=0;
  if(!stereo)*aL=(long long)lv*s;
  else if(surround){long long v;if(lv>=rv){v=(long long)lv*s;*aL=v;*aR=-v;}else{v=(long long)rv*s;*aL=-v;*aR=v;}}
  else{*aL=(long long)lv*s;*aR=(long long)rv*s;}
}
static long long clamp(long long v,long long lo,long long hi){return v<lo?lo:v>hi?hi:v;}

/* one output frame for non-HQ: returns left/right ints, stage 0/1/2 */
static void frame_noshq(RC*o,RC*n,int ol,int orr,int nl,int nr,int stereo,int surround,int stage,long long*L,long long*R,int N,int f){
  long long aL,aR,bL,bR,cL,cR; long long wj=f<N?f:N,wo=f<N?N-f:0,den=N?N:1;
  acc(o,ol,orr,stereo,surround,&aL,&aR);
  acc(n,nl,nr,stereo,surround,&bL,&bR);
  cL=(wo*aL+wj*bL)/den; cR=(wo*aR+wj*bR)/den;
  if(stage==0){*L=clamp(cL>>9,-32768,32767); if(stereo)*R=clamp(cR>>9,-32768,32767); else *R=0;}
  else if(stage==1){*L=clamp(cL>>17,-128,127)+128; if(stereo)*R=clamp(cR>>17,-128,127)+128; else *R=0;}
  else if(stage==2){*L=cL;*R=cR;}
  else {*L=0;*R=0;}
}
/* HQ output frame: 4 internal steps; combined per internal then quantized */
static void frame_hq(RC*o,RC*n,int ol,int orr,int nl,int nr,int stereo,int surround,int stage,long long*L,long long*R,int N,int f){
  long long wj=f<N?f:N,wo=f<N?N-f:0,den=N?N:1; int k;
  long long sl=0,sr=0;
  for(k=0;k<4;k++){
    long long aL,aR,bL,bR,cL,cR;
    acc(o,ol,orr,stereo,surround,&aL,&aR);
    acc(n,nl,nr,stereo,surround,&bL,&bR);
    cL=(wo*aL+wj*bL)/den; cR=(wo*aR+wj*bR)/den;
    if(getenv("DUMPK")&&getenv("DUMPK")[0]=='1'){
      fprintf(stderr,"f=%d k=%d cL=%lld cR=%lld\n",f,k,cL,cR);
    }
    if(stage==0){
      sl+=clamp(cL>>9,-32768,32767);
      sr+=clamp(cR>>9,-32768,32767);
    } else if(stage==1){
      sl+=clamp(cL>>17,-128,127);
      sr+=clamp(cR>>17,-128,127);
    }
  }
  if(stage==0){*L=sl>>2;if(stereo)*R=sr>>2;else *R=0;}
  else if(stage==1){*L=(sl>>2)+128;if(stereo)*R=(sr>>2)+128;else *R=0;}
}

/* run real */
static short rout[32768]; static float fout[32768];
static void run(int hq,int nfade,int stage,int stereo,int surround,int pan,int frq,
                unsigned aflags,int aloop,int aloopend,unsigned bflags,int bloop,int bloopend,int starta,int startb,int nout,int warmframes){
  MikMod_RegisterDriver(&drv_nos);
  MikMod_SetRetriggerFade((ULONG)nfade);
  UWORD m=DMODE_SOFT_SNDFX|DMODE_INTERP; if(hq)m|=DMODE_HQMIXER; if(stereo)m|=DMODE_STEREO;
  if(stage==0)m|=DMODE_16BITS; if(stage==2)m|=DMODE_FLOAT; if(surround)m|=DMODE_SURROUND;
  md_mode=m;md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char rawa[2048],rawb[2048]; int n=512,i;
  for(i=0;i<n;i++){short v=(short)(((i%17)-8)*1500);rawa[2*i]=(unsigned char)(v&255);rawa[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
    short y=(short)(500-i*3);rawb[2*i]=(unsigned char)(y&255);rawb[2*i+1]=(unsigned char)(((unsigned short)y)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)rawa,1024,44100,1,SF_16BITS|SF_SIGNED);
  SAMPLE*b=Sample_LoadRawMem((char*)rawb,1024,44100,1,SF_16BITS|SF_SIGNED);
  a->flags=aflags; a->loopstart=aloop; a->loopend=aloopend;
  b->flags=bflags; b->loopstart=bloop; b->loopend=bloopend;
  MikMod_EnableOutput();
  Voice_SetVolume(0,256);Voice_SetPanning(0,pan);Voice_SetFrequency(0,frq);
  Voice_Play(0,a,starta);
  { static short warm[16384]; VC_WriteBytes((SBYTE*)warm,(ULONG)(warmframes*(stage==2?4:stage==0?2:1)*(stereo?2:1))); }
  Voice_Play(0,b,startb);
  { int bytes=nout*(stage==2?4:stage==0?2:1)*(stereo?2:1);
    if(stage==2) VC_WriteBytes((SBYTE*)fout,bytes); else VC_WriteBytes((SBYTE*)rout,bytes); }
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}

/* build padded sample from raw generator */
static void gen(short*pad,int n,int which,int loopend){int i;for(i=0;i<n;i++){pad[i]=which? (short)(500-i*3):(short)(((i%17)-8)*1500);}for(i=0;i<16;i++)pad[n+i]=pad[(loopend?i%loopend:i)%n];}

static int compare_one(int hq,int nfade,int stage,int stereo,int surround,int pan,int frq,
                       unsigned aflags,int aloop,int aloopend,unsigned bflags,int bloop,int bloopend,
                       int starta,int startb,int nout,int warmframes,int verbose){
  short apad[2048],bpad[2048]; int i; RC o,nn; int ol,orr,nl,nr;
  long long inc_o = hq ? (long long)(((long long)frq<<(28-2))/44100) : (long long)(((long long)frq<<11)/44100);
  long long inc_n = hq ? inc_o : (long long)(((long long)frq<<11)/44100);
  long long pos_o;
  if(getenv("DIAG")) fprintf(stderr,"pos_o=%lld inc_o=%lld\n",pos_o,inc_o);
  int fb = hq?28:11;
  pos_o = ((long long)starta<<fb)+((aflags&SF_REVERSE)?-1:1)*(long long)warmframes*(hq?4:1)*inc_o;
  gen(apad,512,0,aloopend?aloopend:512); gen(bpad,512,1,bloopend?bloopend:512);
  rinit(&o,apad,512,aflags,aloop,aloopend,pos_o,(aflags&SF_REVERSE)?-inc_o:inc_o,fb,hq);
  rinit(&nn,bpad,512,bflags,bloop,bloopend,(long long)startb<<fb,inc_n,fb,hq);
  { int esurr = surround && pan==PAN_SURROUND;
  gains(256,pan,stereo,esurr,hq,&ol,&orr);
  gains(256,pan,stereo,esurr,hq,&nl,&nr);
  surround=esurr; }
  run(hq,nfade,stage,stereo,surround,pan,frq,aflags,aloop,aloopend,bflags,bloop,bloopend,starta,startb,nout,warmframes);
  int bad=0;
  for(i=0;i<nout;i++){
    long long L=0,R=0;
    if(hq) frame_hq(&o,&nn,ol,orr,nl,nr,stereo,surround,stage,&L,&R,nfade,i);
    else frame_noshq(&o,&nn,ol,orr,nl,nr,stereo,surround,stage,&L,&R,nfade,i);
    int gotL,gotR;
    if(stage==2){ double gl=fout[i*(stereo?2:1)], gr=stereo?fout[i*(stereo?2:1)+1]:0; double ys=524288.0; double ml=(double)(L>>5)/ys, mr=(double)(R>>5)/ys;
      if(fabs(gl-ml)>1e-4||fabs(gr-mr)>1e-4){ if(verbose&&bad<5)printf("  f[%d] real %.6f/%.6f mdl %.6f/%.6f\n",i,gl,gr,ml,mr); bad++; } }
    else {
      if(stage==0){gotL=stereo?rout[i*2]:rout[i];gotR=stereo?rout[i*2+1]:0;}
      else {gotL=(unsigned char)((SBYTE*)rout)[i*(stereo?2:1)];gotR=stereo?(unsigned char)((SBYTE*)rout)[i*(stereo?2:1)+1]:0;}
      if(gotL!=L||gotR!=R){ long long d=llabs((long long)gotL-L); if(d>bad)bad=(int)d; if(verbose&&d>1)printf("  [%d] real %d/%d mdl %lld/%lld diff %lld\n",i,gotL,gotR,L,R,d); }
    }
  }
  if(verbose){printf("real0..3: %d %d %d %d %d %d %d %d\n",rout[0],rout[1],rout[2],rout[3],rout[4],rout[5],rout[6],rout[7]);} 
  if(verbose)printf("hq=%d stage=%d st=%d surr=%d pan=%d frq=%d af=%x al=%d/%d bf=%x bl=%d startb=%d N=%d : bad=%d/%d\n",
     hq,stage,stereo,surround,pan,frq,aflags,aloop,aloopend,bflags,bloop,startb,nfade,bad,nout);
  return bad;
}

int main(void){
  int t=0;
  /* non-HQ: exact expected */
  t+=compare_one(0,8,0,1,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,0);
  t+=compare_one(0,8,0,1,0,200,30800,SF_LOOP,0,512,SF_LOOP,0,512,0,7,40,64,1);
  t+=compare_one(0,8,0,0,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,1);
  t+=compare_one(0,8,1,1,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,1);
  t+=compare_one(0,8,1,0,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,1);
  t+=compare_one(0,8,2,1,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,1);
  t+=compare_one(0,16,0,1,1,512,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,1);
  t+=compare_one(0,16,0,1,1,60,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,1);
  t+=compare_one(0,16,0,1,0,128,22050,SF_LOOP|SF_BIDI,300,512,SF_LOOP,0,512,0,0,60,64,1);
  t+=compare_one(0,16,0,1,0,128,22050,SF_LOOP|SF_REVERSE,0,512,SF_LOOP,0,512,500,0,60,64,1);
  t+=compare_one(0,16,0,1,0,128,22050,SF_LOOP,10,300,SF_LOOP,0,512,0,0,80,64,1);
  t+=compare_one(0,16,0,1,0,128,22050,0,0,512,0,0,320,300,0,60,4,1);
  t+=compare_one(0,16,0,1,0,128,22050,SF_LOOP,0,512,0,0,320,0,0,60,64,1);
  t+=compare_one(0,32,1,1,0,128,22050,SF_LOOP,0,512,0,0,320,0,0,60,8,1);
  printf("nonhq bad total %d\n",t);
  { int hqbad=0;
  hqbad+=compare_one(1,8,0,1,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,0);
  hqbad+=compare_one(1,8,0,1,0,200,30800,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,0);
  hqbad+=compare_one(1,8,1,1,0,128,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,0);
  hqbad+=compare_one(1,8,0,1,1,512,22050,SF_LOOP,0,512,SF_LOOP,0,512,0,0,40,64,0);
  hqbad+=compare_one(1,16,0,1,0,128,22050,SF_LOOP|SF_BIDI,300,512,SF_LOOP,0,512,0,0,60,64,0);
  printf("hq total mismatches (1 unit allowed) %d\n",hqbad); }
  return 0;
}
