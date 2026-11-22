#include <SDL3_sound/SDL_sound.h>
#include "task_support/wave_fixture.h"
#include <stdio.h>
#include <string.h>
static unsigned char b[40000];static short ref[1024],out[4096];
int main(void){if(!SDL_Init(0)||!Sound_Init())return 1;size_t n=wave_pcm(b,256,32000);SDL_AudioSpec want;want.format=SDL_AUDIO_S16;want.channels=2;want.freq=32000;
Sound_Sample *s=Sound_NewSample(SDL_IOFromConstMem(b,n),"WAV",&want,28);if(!s)return 2;unsigned done=0;while(!(s->flags&SOUND_SAMPLEFLAG_EOF)){unsigned z=Sound_Decode(s);if(z+done>sizeof(ref))return 3;memcpy((char*)ref+done,s->buffer,z);done+=z;}Sound_FreeSample(s);if(done!=1024)return 4;
s=Sound_NewSample(SDL_IOFromConstMem(b,n),"WAV",&want,28);if(!s||!Sound_SetLoop(s,5,23,2))return 5;Sound_LoopPosition p;if(!Sound_GetLoopPosition(s,&p)||p.logical_frame||p.output_frame||p.remaining_repeats!=2)return 6;
done=0;while(!(s->flags&SOUND_SAMPLEFLAG_EOF)){unsigned z=Sound_Decode(s);if(s->flags&SOUND_SAMPLEFLAG_ERROR||done+z>sizeof(out))return 7;memcpy((char*)out+done,s->buffer,z);done+=z;}
if(done!=(256+36)*4)return 8;for(unsigned q=0;q<292;q++){unsigned src=q<23?q:q<59?5+(q-23)%18:q-36;for(unsigned c=0;c<2;c++)if(out[q*2+c]!=ref[src*2+c])return 9;}
if(!Sound_SeekLoopFrame(s,24)||!Sound_GetLoopPosition(s,&p)||p.logical_frame!=24||p.source_frame!=6||p.output_frame!=0||p.remaining_repeats!=2)return 10;
unsigned z=Sound_Decode(s);if(z!=28||memcmp(s->buffer,out+24*2,z))return 11;if(!Sound_GetLoopPosition(s,&p)||p.logical_frame!=31||p.output_frame!=7||p.source_frame!=13)return 12;
if(Sound_SeekLoopFrame(s,293)||!Sound_GetLoopPosition(s,&p)||p.logical_frame!=31)return 13;Sound_FreeSample(s);Sound_Quit();SDL_Quit();puts("LOOP_API_NORMAL_OK pcm=292 seek=24 source=6 delivered=7");return 0;}
