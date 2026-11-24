#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
struct TaskSampleSource : juce::AudioSource {
 juce::int64 position=0; double preparedRate=0; int reads=0;
 static float sample(juce::int64 n,int c){return float(.17*std::sin(n*.017+(c*.3))+.0007*(n%71));}
 void prepareToPlay(int,double r) override{position=0;preparedRate=r;}
 void releaseResources() override{}
 void getNextAudioBlock(const juce::AudioSourceChannelInfo&i) override{++reads;for(int c=0;c<i.buffer->getNumChannels();++c)for(int n=0;n<i.numSamples;++n)i.buffer->setSample(c,i.startSample+n,sample(position+n,c));position+=i.numSamples;}
};
