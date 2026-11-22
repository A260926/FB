
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <stdio.h>
#include <math.h>
#define REQUIRE(x) do { if (!(x)) {fprintf(stderr,"baseline:%d %s\n",__LINE__,SDL_GetError());return 1;} } while(0)
static int calls;
static void SDLCALL post(void *u,MIX_Mixer *m,const SDL_AudioSpec *s,float *p,int n) {
 (void)u;(void)m; if(s->channels!=2) return;
 for(int i=0;i<n;++i) p[i]*=0.5f;
 ++calls;
}
int main(void) {
 REQUIRE(SDL_Init(0)); REQUIRE(MIX_Init());
 SDL_AudioSpec spec={SDL_AUDIO_F32,2,48000};
 MIX_Mixer *m=MIX_CreateMixer(&spec); REQUIRE(m);
 float a[8192],b[8192],out[1536];
 for(int i=0;i<4096;++i){a[2*i]=0.125f;a[2*i+1]=-0.25f;b[2*i]=0.0625f;b[2*i+1]=0.125f;}
 MIX_Audio *aa=MIX_LoadRawAudio(m,a,sizeof(a),&spec),*bb=MIX_LoadRawAudio(m,b,sizeof(b),&spec); REQUIRE(aa&&bb);
 MIX_Track *ta=MIX_CreateTrack(m),*tb=MIX_CreateTrack(m);REQUIRE(ta&&tb);
 REQUIRE(MIX_SetTrackAudio(ta,aa));REQUIRE(MIX_SetTrackAudio(tb,bb));
 REQUIRE(MIX_SetMixerGain(m,0.75f));REQUIRE(MIX_SetPostMixCallback(m,post,NULL));
 REQUIRE(MIX_PlayTrack(ta,0));REQUIRE(MIX_PlayTrack(tb,0));
 int bytes=MIX_Generate(m,out,sizeof(out));REQUIRE(bytes==(int)sizeof(out));REQUIRE(calls>0);
 for(int i=0;i<768;++i){REQUIRE(fabsf(out[2*i]-0.0703125f)<1e-6f);REQUIRE(fabsf(out[2*i+1]+0.046875f)<1e-6f);}
 printf("PLAYBACK_BASELINE_OK frames=768 left=%.7f right=%.7f postmix=%d\n",out[0],out[1],calls);
 MIX_DestroyMixer(m);MIX_DestroyAudio(aa);MIX_DestroyAudio(bb);MIX_Quit();SDL_Quit();return 0;
}
