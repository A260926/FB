#pragma once
#include "ISound.h"
#include "IReader.h"
#include <algorithm>
#include <memory>
// Deterministic input fixture; no ducking implementation.
class ProbeReader : public aud::IReader {
    int p=0; float v;
public:
    explicit ProbeReader(float value):v(value){}
    bool isSeekable() const override {return true;}
    void seek(int n) override {p=std::max(0,n);}
    int getLength() const override {return 4096;}
    int getPosition() const override {return p;}
    aud::Specs getSpecs() const override {return {8000,aud::CHANNELS_MONO};}
    void read(int& n,bool& eos,aud::sample_t* b) override {
        n=std::min(n,std::max(0,4096-p));std::fill(b,b+n,v);p+=n;eos=(p==4096);
    }
};
class ProbeSound : public aud::ISound {
    float v;
public:
    explicit ProbeSound(float value):v(value){}
    std::shared_ptr<aud::IReader> createReader() override {return std::make_shared<ProbeReader>(v);}
};
