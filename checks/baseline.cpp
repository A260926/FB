// Starting-state witness. This uses only the existing constant-rate API.
#include "RateTransposer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace soundtouch;
static void check(bool b, const char *s) { if (!b) { std::fprintf(stderr,"FAIL %s\n",s); std::exit(1); } }
static void collect(RateTransposer &r, std::vector<float> &v, unsigned width=4096) {
    std::vector<float> buf(width*2);
    while (r.numSamples()) { unsigned n=r.receiveSamples(buf.data(),width); v.insert(v.end(),buf.begin(),buf.begin()+2*n); }
}
static std::vector<float> render(unsigned block, bool aa, bool stepped, float rate, double freq) {
    TransposerBase::setAlgorithm(TransposerBase::LINEAR);
    RateTransposer r; r.setChannels(2); r.enableAAFilter(aa); r.setRate(rate);
    const unsigned n=48000;
    std::vector<float> input(n*2), out;
    for(unsigned i=0;i<n;i++) { float y=0.4f*std::sin(6.283185307179586*freq*i);input[2*i]=y; input[2*i+1]=-0.5f*y; }
    for(unsigned p=0;p<n;) {
        unsigned k=std::min(block,n-p);
        // Typical caller-side approximation: rate updates tied to input blocks.
        if(stepped) r.setRate(0.75f+0.75f*float(p)/n);
        r.putSamples(input.data()+2*p,k); collect(r,out,257); p+=k;
    }
    check(out.size()>20000,"real output");
    for(unsigned i=0;i<out.size();i+=2)check(std::isfinite(out[i]) && std::fabs(out[i+1]+0.5f*out[i])<1e-6,"channel relation");
    return out;
}
static double rms(const std::vector<float>&v) {double s=0;unsigned n=0;for(unsigned i=2048;i+2048<v.size();i+=2){s+=v[i]*v[i];n++;}return std::sqrt(s/n);}
static unsigned le32(const unsigned char *p) { return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24); }
int main(int argc,char**argv) {
    if(argc==3) {
        FILE *f=std::fopen(argv[2],"rb");check(f,"open output WAV");unsigned char h[44];check(std::fread(h,1,44,f)==44,"read output WAV");
        check(h[0]=='R' && h[8]=='W' && h[22]==2 && h[32]==4 && h[34]==16,"output format");
        check(le32(h+24)==48000,"output sample rate");unsigned frames=le32(h+40)/4;check(frames>39500 && frames<40500,"tempo output duration");
        long long energy=0;double relation=0;unsigned char b[4];unsigned read=0;
        while(std::fread(b,1,4,f)==4){int l=short(unsigned(b[0])|(unsigned(b[1])<<8)),r=short(unsigned(b[2])|(unsigned(b[3])<<8));energy+=(long long)l*l;relation=std::max(relation,std::fabs(double(r)+0.5*l));read++;}
        check(read==frames && energy>1000000 && relation<=2,"output PCM/channel relation");std::fclose(f);
        std::printf("OLD_SOUNDSTRETCH_OK frames=%u channels=2 rate=48000\n",frames);return 0;
    }
    if(argc>1) {
        FILE *f=std::fopen(argv[1],"wb");check(f,"open WAV");
        // One second, 48 kHz, stereo s16le WAV for the unmodified SoundStretch app.
        unsigned char h[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0x80,0xbb,0,0,0,0xee,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
        unsigned bytes=48000*4, riff=bytes+36;
        for(unsigned i=0;i<4;i++){h[4+i]=(riff>>(8*i))&255;h[40+i]=(bytes>>(8*i))&255;}
        check(std::fwrite(h,1,44,f)==44,"WAV header");
        for(unsigned i=0;i<48000;i++){short a=short(10000*std::sin(6.283185307179586*440*i/48000));unsigned short b=(unsigned short)a,c=(unsigned short)(-a/2);unsigned char v[4]={(unsigned char)b,(unsigned char)(b>>8),(unsigned char)c,(unsigned char)(c>>8)};check(std::fwrite(v,1,4,f)==4,"WAV payload");}
        std::fclose(f); return 0;
    }
    auto a=render(4096,true,false,0.75f,440.0/48000),b=render(37,true,false,0.75f,440.0/48000);
    check(a.size()==b.size(),"constant frame counts");double err=0;for(unsigned i=0;i<a.size();i++)err=std::max(err,double(std::fabs(a[i]-b[i])));
    check(err<1e-6,"constant chunk equivalence");
    std::printf("CONSTANT_RATE_OK frames=%zu max_error=%.9g\n",a.size()/2,err);
    auto high=render(4096,true,false,1.5f,0.4), highRaw=render(4096,false,false,1.5f,0.4);
    double suppression=20*std::log10(rms(high)/rms(highRaw));check(suppression < -35,"AA suppression");
    std::printf("AA_CONSTANT_OK suppression_db=%.3f\n",suppression);
    auto coarse=render(4096,true,true,0.75f,440.0/48000),fine=render(37,true,true,0.75f,440.0/48000);
    double mse=0;unsigned common=std::min(coarse.size(),fine.size());for(unsigned i=2048;i<common;i+=2){double d=coarse[i]-fine[i];mse+=d*d;}
    double delta=std::sqrt(mse/((common-2048)/2));check(delta>0.05,"stepped caller approximation is block dependent");
    std::printf("CALLER_STEPS_DIFFER coarse_frames=%zu fine_frames=%zu rms_difference=%.6f\n",coarse.size()/2,fine.size()/2,delta);
    std::puts("STARTING_STATE_WITNESS_OK");
}
