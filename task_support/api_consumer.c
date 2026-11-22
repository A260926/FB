#include <SDL3/SDL.h>
#include <stdio.h>
#define C(x) do{if(!(x)){fprintf(stderr,"TAP_API_FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 SDL_AudioSpec f={SDL_AUDIO_F32,1,48000};float x[]={0.125f,-0.25f,0.375f,-0.5f},a[4],b[4];
 SDL_AudioStream *p=SDL_CreateAudioStream(&f,&f);C(p);
 SDL_AudioStream *t=SDL_CreateAudioStreamTap(p,&f);C(t&&t!=p);
 C(SDL_PutAudioStreamData(p,x,sizeof(x)));C(SDL_FlushAudioStream(p));
 C(SDL_GetAudioStreamData(p,a,sizeof(a))==sizeof(a));C(SDL_GetAudioStreamData(t,b,sizeof(b))==sizeof(b));
 for(int i=0;i<4;i++){C(a[i]==x[i]);C(b[i]==x[i]);}
 C(!SDL_PutAudioStreamData(t,x,sizeof(x)));SDL_DestroyAudioStream(p);SDL_DestroyAudioStream(t);
 puts("AUDIO_TAP_API_OK");return 0;
}
