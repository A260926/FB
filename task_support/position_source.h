#pragma once
#include "task_support/sample_source.h"
struct TaskPositionSource : juce::PositionableAudioSource {
 juce::int64 position=0;
 int id;
 explicit TaskPositionSource(int value):id(value){}
 static float sample(juce::int64 n,int c,int id){return float(.11*std::sin(n*.031+id*.23+c*.41)+.001*(n%97)+.025*id);}
 void prepareToPlay(int,double) override {position=0;}
 void releaseResources() override {}
 void getNextAudioBlock(const juce::AudioSourceChannelInfo& i) override {
  for(int c=0;c<i.buffer->getNumChannels();++c)for(int n=0;n<i.numSamples;++n)
   i.buffer->setSample(c,i.startSample+n,position+n<getTotalLength()?sample(position+n,c,id):0.f);
  position+=i.numSamples;
 }
 void setNextReadPosition(juce::int64 p) override {position=p;}
 juce::int64 getNextReadPosition() const override {return position;}
 juce::int64 getTotalLength() const override {return 20000;}
 bool isLooping() const override {return false;}
};
