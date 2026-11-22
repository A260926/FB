#include <SDL3/SDL.h>
#include <stdio.h>
#include <math.h>
static int freed;
static void SDLCALL done(void *u,const void *p,int n){(void)u;(void)p;(void)n;freed++;}
#define C(x) do {if(!(x)){fprintf(stderr,"BASELINE_FAIL %d %s: %s\n",__LINE__,#x,SDL_GetError());return 1;}}while(0)
int main(void){
 SDL_AudioSpec a={SDL_AUDIO_F32,2,48000}, b={SDL_AUDIO_F32,2,32000};
 float in[4096],out[4096],ref[4096];int i,n=0,m=0,k;
 for(i=0;i<4096;i++)in[i]=(float)(((i*17)%97)-48)/128;
 SDL_AudioStream *s=SDL_CreateAudioStream(&a,&a);C(s);
 C(SDL_PutAudioStreamDataNoCopy(s,in,sizeof(in),done,NULL));C(freed==0);C(SDL_FlushAudioStream(s));
 C(SDL_GetAudioStreamData(s,out,sizeof(out))==sizeof(out));
 for(i=0;i<4096;i++)C(in[i]==out[i]);SDL_DestroyAudioStream(s);C(freed==1);
 SDL_AudioStream *t=SDL_CreateAudioStream(&a,&b);s=SDL_CreateAudioStream(&a,&b);C(s&&t);
 C(SDL_PutAudioStreamData(s,in,sizeof(in)));C(SDL_PutAudioStreamData(t,in,sizeof(in)));
 C(SDL_FlushAudioStream(s)&&SDL_FlushAudioStream(t));
 k=SDL_GetAudioStreamData(s,ref,sizeof(ref));C(k>0);n=k/sizeof(float);
 while((k=SDL_GetAudioStreamData(t,out+m,14*sizeof(float)))>0)m+=k/sizeof(float);
 C(k==0&&m==n&&m>2000&&m<3000);for(i=0;i<m;i++)C(fabsf(out[i]-ref[i])<2e-6f);
 SDL_DestroyAudioStream(s);SDL_DestroyAudioStream(t);
 printf("SDL_BASELINE_OK nocopy_callbacks=%d output_frames=%d split_equal=1\n",freed,m/2);return 0;
}
