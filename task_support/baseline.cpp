#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
int main(){
 ALCdevice*d=alcLoopbackOpenDeviceSOFT(nullptr);if(!d)return 1;
 const ALCint attrs[]={ALC_FREQUENCY,48000,ALC_FORMAT_CHANNELS_SOFT,ALC_STEREO_SOFT,ALC_FORMAT_TYPE_SOFT,ALC_FLOAT_SOFT,0};
 ALCcontext*c=alcCreateContext(d,attrs);if(!c||!alcMakeContextCurrent(c))return 2;
 ALuint b=0,s=0,e=0,slot=0,f=0;alGenBuffers(1,&b);alGenSources(1,&s);alGenEffects(1,&e);alGenAuxiliaryEffectSlots(1,&slot);alGenFilters(1,&f);
 alEffecti(e,AL_EFFECT_TYPE,AL_EFFECT_ECHO);alEffectf(e,AL_ECHO_DELAY,0.01f);alEffectf(e,AL_ECHO_LRDELAY,0.f);alEffectf(e,AL_ECHO_DAMPING,0.f);alEffectf(e,AL_ECHO_FEEDBACK,0.f);alEffectf(e,AL_ECHO_SPREAD,0.f);alAuxiliaryEffectSloti(slot,AL_EFFECTSLOT_EFFECT,e);
 alFilteri(f,AL_FILTER_TYPE,AL_FILTER_LOWPASS);alFilterf(f,AL_LOWPASS_GAIN,0.f);alSourcei(s,AL_DIRECT_FILTER,f);alSource3i(s,AL_AUXILIARY_SEND_FILTER,slot,0,AL_FILTER_NULL);alSourcei(s,AL_SOURCE_RELATIVE,AL_TRUE);alSourcef(s,AL_ROLLOFF_FACTOR,0.f);
 std::vector<float> input(4096);input[2048]=0.1f;alBufferData(b,AL_FORMAT_MONO_FLOAT32,input.data(),input.size()*sizeof(float),48000);alSourcei(s,AL_BUFFER,b);
 if(alGetError()!=AL_NO_ERROR)return 3;
 alSourcePlay(s);std::vector<float> output(4096*2);alcRenderSamplesSOFT(d,output.data(),4096);
 unsigned first=4096,last=0;double energy=0;
 for(unsigned i=0;i<4096;++i)for(unsigned ch=0;ch<2;++ch){float x=output[2*i+ch];if(!std::isfinite(x))return 4;if(std::fabs(x)>1e-5){first=std::min(first,i);last=std::max(last,i);}energy+=double(x)*x;}
 // First Echo tap includes the upstream's one-sample delay offset.
 if(first<2527||first>2530||last<first||energy<1e-5||energy>0.05){std::fprintf(stderr,"unexpected Echo first=%u last=%u energy=%g\n",first,last,energy);return 5;}
 std::printf("BASELINE LOOPBACK_ECHO first=%u last=%u energy=%.9g PCM_OK\n",first,last,energy);
 alSource3i(s,AL_AUXILIARY_SEND_FILTER,AL_EFFECTSLOT_NULL,0,AL_FILTER_NULL);alSourcei(s,AL_BUFFER,0);alDeleteSources(1,&s);alDeleteBuffers(1,&b);alDeleteFilters(1,&f);alDeleteAuxiliaryEffectSlots(1,&slot);alDeleteEffects(1,&e);
 if(alGetError()!=AL_NO_ERROR||alcGetError(d)!=ALC_NO_ERROR)return 6;
 alcMakeContextCurrent(nullptr);alcDestroyContext(c);if(!alcCloseDevice(d))return 7;
 std::puts("BASELINE CLEANUP_OK");
}
