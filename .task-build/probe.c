#include <SDL3/SDL.h>
#include <stdio.h>
static int dones;
static void SDLCALL cb(void *u, const void *p, int n){(void)u;(void)p;(void)n;++dones;}
int main(void){
    SDL_AudioSpec a={SDL_AUDIO_F32,2,48000};
    float in[80], out[80];
    for(int i=0;i<80;i++) in[i]=i*0.01f;
    SDL_AudioStream *s=SDL_CreateAudioStream(&a,&a);
    SDL_PutAudioStreamDataNoCopy(s,in,sizeof(in),cb,NULL);
    SDL_FlushAudioStream(s);
    printf("after flush dones=%d\n",dones);
    int g=SDL_GetAudioStreamData(s,out,sizeof(out));
    printf("get=%d dones=%d\n",g,dones);
    g=SDL_GetAudioStreamData(s,out,64);
    printf("get2=%d dones=%d\n",g,dones);
    SDL_DestroyAudioStream(s);
    printf("after destroy dones=%d\n",dones);
    return 0;
}
