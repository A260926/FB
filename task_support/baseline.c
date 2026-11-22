#include <SDL3_sound/SDL_sound.h>
#include "task_support/wave_fixture.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
static unsigned char b[40000];
static int decode(size_t bytes,int convert,unsigned expected){SDL_AudioSpec want;want.format=convert?SDL_AUDIO_F32:SDL_AUDIO_S16;want.channels=2;want.freq=convert?48000:32000;
Sound_Sample *s=Sound_NewSample(SDL_IOFromConstMem(b,bytes),"WAV",&want,128);if(!s){fprintf(stderr,"open: %s\n",Sound_GetError());return 1;}unsigned total=0;double e=0;for(int k=0;k<10000;k++){unsigned n=Sound_Decode(s);if(s->flags&SOUND_SAMPLEFLAG_ERROR)return 2;if(convert){float *v=s->buffer;for(unsigned i=0;i<n/4;i++)e+=fabs(v[i]);}else{int16_t *v=s->buffer;for(unsigned i=0;i<n/2;i++)e+=abs(v[i]);}total+=n;if(s->flags&SOUND_SAMPLEFLAG_EOF)break;if(k==9999)return 3;}
unsigned frames=total/(convert?8:4);Sound_FreeSample(s);if(e==0||(!convert&&frames!=expected)||(!frames))return 4;if(convert)printf("WAV_CONVERT frames=%u nonzero=1\n",frames);return 0;}
int main(void){if(!SDL_Init(0)||!Sound_Init())return 1;size_t n=wave_pcm(b,512,32000);int r=decode(n,0,512);if(r)return r+10;n=wave_adpcm(b,8,32000);r=decode(n,0,160);if(r)return r+20;n=wave_pcm(b,512,32000);r=decode(n,1,0);if(r)return r+30;Sound_Quit();SDL_Quit();puts("WAV_BASELINE pcm_frames=512 adpcm_frames=160");return 0;}
