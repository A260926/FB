#include "task_support/sample_source.h"
#include <cstdio>
int main(){
 TaskSampleSource source;juce::ResamplingAudioSource resampler(&source,false,2);
 resampler.setResamplingRatio(1.5);resampler.prepareToPlay(64,48000);
 if(source.preparedRate!=72000)return 2;
 juce::AudioBuffer<float> out(2,64);double energy=0;int frames=0;
 for(int b=0;b<16;++b){resampler.getNextAudioBlock({&out,0,64});frames+=64;for(int c=0;c<2;++c)for(int n=0;n<64;++n){float v=out.getSample(c,n);if(!std::isfinite(v))return 3;energy+=v*v;}}
 if(energy<1||source.position<1536||source.reads<=0)return 4;
 resampler.releaseResources();
 TaskSampleSource unity;juce::ResamplingAudioSource direct(&unity,false,1);direct.prepareToPlay(16,48000);juce::AudioBuffer<float> check(1,16);direct.getNextAudioBlock({&check,0,16});
 for(int n=0;n<16;++n)if(std::fabs(check.getSample(0,n)-TaskSampleSource::sample(n,0))>1e-7)return 5;
 direct.releaseResources();std::printf("JUCE_BASELINE_OK output=%d source=%lld reads=%d energy=%.6f identity=16\n",frames,(long long)source.position,source.reads,energy);
return 0;
}
