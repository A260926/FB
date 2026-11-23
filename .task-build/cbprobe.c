#include <SDL3/SDL.h>
#include <stdio.h>
static int n, bytes;
static void SDLCALL cb(void *u, SDL_AudioStream *s, int add, int total){(void)u;(void)s;(void)total;++n;bytes+=add; printf("cb add=%d total=%d\n",add,total);}
int main(void){
    SDL_AudioSpec a={SDL_AUDIO_F32,2,48000};
    SDL_AudioStream *p=SDL_CreateAudioStream(&a,&a);
    SDL_AudioStream *t=SDL_CreateAudioStreamTap(p,&a);
    SDL_SetAudioStreamPutCallback(p,cb,NULL);
    float x[96];
    int r=SDL_PutAudioStreamData(p,x,sizeof x);
    printf("put=%d calls=%d bytes=%d expect=%zu avail=%d\n",r,n,bytes,sizeof x,SDL_GetAudioStreamAvailable(p));
    return 0;
}
