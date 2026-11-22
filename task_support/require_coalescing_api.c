#include "miniaudio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"required coalescing API probe failed: %s\n",#x);return 1;}}while(0)
static void le(FILE *f, unsigned v, unsigned n) {while(n--){fputc(v&255,f);v>>=8;}}
static int sample(unsigned i,unsigned c){return (int)((i*37+c*113)%60001)-30000;}
int main(void) {
    const unsigned frames=300000; unsigned i,c; ma_uint64 n=0,cursor=0; short out[66];
    ma_resource_manager rm; ma_resource_manager_data_stream s; ma_result r;
    ma_resource_manager_config cfg=ma_resource_manager_config_init();
    FILE *f=fopen(".task-build/required-coalescing.wav","wb"); if(!f)return 2;
    fwrite("RIFF",1,4,f);le(f,36+frames*4,4);fwrite("WAVEfmt ",1,8,f);le(f,16,4);le(f,1,2);le(f,2,2);le(f,48000,4);le(f,192000,4);le(f,4,2);le(f,16,2);fwrite("data",1,4,f);le(f,frames*4,4);
    for(i=0;i<frames;i++)for(c=0;c<2;c++)le(f,(unsigned)(uint16_t)sample(i,c),2);
    if(fclose(f))return 3;
    cfg.jobThreadCount=0;cfg.flags=MA_RESOURCE_MANAGER_FLAG_NO_THREADING;cfg.decodedFormat=ma_format_s16;cfg.decodedChannels=2;cfg.decodedSampleRate=48000;
    if(ma_resource_manager_init(&cfg,&rm))return 4;
    if(ma_resource_manager_data_stream_init(&rm,".task-build/required-coalescing.wav",MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_SEEK_COALESCING,NULL,&s))return 5;
    ma_resource_manager_seek_state state;
    REQUIRE(ma_resource_manager_data_stream_get_seek_state(&s,&state)==MA_SUCCESS);
    REQUIRE(state.requested==0&&state.settled==0&&state.frameIndex==0&&state.result==MA_SUCCESS);
    if(ma_resource_manager_data_stream_read_pcm_frames(&s,out,33,&n)||n!=33)return 6;
    for(i=0;i<33;i++)for(c=0;c<2;c++)if(out[i*2+c]!=sample(i,c))return 7;
    if(ma_resource_manager_data_stream_seek_to_pcm_frame(&s,123457))return 8;
    REQUIRE(ma_resource_manager_data_stream_get_seek_state(&s,&state)==MA_SUCCESS);
    REQUIRE(state.requested==1&&state.settled==0&&state.frameIndex==123457&&state.result==MA_BUSY);
    n=99;r=ma_resource_manager_data_stream_read_pcm_frames(&s,out,33,&n);
    if(r!=MA_BUSY||n!=0)return 9;
    for(unsigned seek_jobs=0;state.result==MA_BUSY;++seek_jobs){
        REQUIRE(seek_jobs<64);
        REQUIRE(ma_resource_manager_process_next_job(&rm)==MA_SUCCESS);
        REQUIRE(ma_resource_manager_data_stream_get_seek_state(&s,&state)==MA_SUCCESS);
    }
    REQUIRE(state.requested==1&&state.settled==1&&state.frameIndex==123457&&state.result==MA_SUCCESS);
    if(ma_resource_manager_data_stream_get_cursor_in_pcm_frames(&s,&cursor)||cursor!=123457)return 11;
    if(ma_resource_manager_data_stream_read_pcm_frames(&s,out,33,&n)||n!=33)return 12;
    for(i=0;i<33;i++)for(c=0;c<2;c++)if(out[i*2+c]!=sample(123457+i,c))return 13;
    printf("REQUIRED_COALESCING_PCM_OK frames=%llu cursor=%llu busy_frames=0\n",(unsigned long long)n,(unsigned long long)cursor);
    {
        ma_engine e; ma_sound sound; ma_engine_config ec=ma_engine_config_init();
        float pcm[130]; double energy=0; ma_uint64 rendered=0;
        ec.noDevice=MA_TRUE;ec.channels=2;ec.sampleRate=48000;ec.pResourceManager=&rm;
        if(ma_engine_init(&ec,&e))return 14;
        if(ma_sound_init_from_data_source(&e,(ma_data_source*)&s,MA_SOUND_FLAG_NO_SPATIALIZATION,NULL,&sound))return 15;
        ma_sound_set_pitch(&sound,1.25f);
        if(ma_sound_start(&sound)||ma_engine_read_pcm_frames(&e,pcm,65,&rendered)||rendered!=65)return 16;
        for(i=0;i<130;i++){if(!isfinite(pcm[i]))return 17;energy+=(double)pcm[i]*pcm[i];}
        if(energy<=0||ma_sound_get_data_source(&sound)!=(ma_data_source*)&s)return 18;
        printf("REQUIRED_COALESCING_SOUND_OK frames=%llu pitch=1.25 nonzero=1\n",(unsigned long long)rendered);
        ma_sound_uninit(&sound);ma_engine_uninit(&e);
    }
    ma_resource_manager_data_stream_uninit(&s);ma_resource_manager_uninit(&rm);remove(".task-build/required-coalescing.wav");return 0;
}
