#include "ALACEncoder.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <vector>
int main() {
    const unsigned N=4096,C=2;
    AudioFormatDescription input={},output={};
    input.mSampleRate=output.mSampleRate=44100;
    input.mChannelsPerFrame=output.mChannelsPerFrame=C;
    input.mFormatID=kALACFormatLinearPCM; input.mBitsPerChannel=16;
    input.mBytesPerPacket=input.mBytesPerFrame=C*2; input.mFramesPerPacket=1;
    output.mFormatID=kALACFormatAppleLossless; output.mFormatFlags=1;
    ALACEncoder enc; enc.SetFrameSize(N); assert(!enc.InitializeEncoder(output));
    unsigned char cookie[128]={0}; uint32_t cookieSize=sizeof(cookie); enc.GetMagicCookie(cookie,&cookieSize);
    ALACDecoder dec; assert(!dec.Init(cookie,cookieSize));
    std::vector<short> pcm(N*C),decoded(N*C);
    std::vector<unsigned char> packet(N*C*8+64);
    unsigned total=0; uint32_t hash=2166136261u;
    for(unsigned f=0;f<3;f++) {
        for(unsigned i=0;i<N;i++) { double t=i+f*N; pcm[2*i]=(short)(11000*sin(t*0.071)+3000*sin(t*0.113)); pcm[2*i+1]=(short)(9000*sin(t*0.071+0.4)+2200*cos(t*0.037)); }
        int32_t bytes=N*C*2; assert(!enc.Encode(input,output,(unsigned char*)pcm.data(),packet.data(),&bytes)); assert(bytes>0);
        total+=bytes; for(int i=0;i<bytes;i++) hash=(hash^packet[i])*16777619u;
        BitBuffer bits; BitBufferInit(&bits,packet.data(),bytes); uint32_t got=0;
        assert(!dec.Decode(&bits,(unsigned char*)decoded.data(),N,C,&got)); assert(got==N);
        assert(!memcmp(pcm.data(),decoded.data(),N*C*2));
    }
    printf("ALAC_BASELINE_OK packets=3 samples=12288 bytes=%u fnv=%08x\n",total,hash);
}
