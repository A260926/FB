#ifndef MOD_FIXTURE_H
#define MOD_FIXTURE_H
#include <vector>
#include <cstring>
// Original generated ProTracker fixture: one 64-row pattern and one looped ramp.
static std::vector<unsigned char> make_mod() {
    std::vector<unsigned char> d(1084+1024+128,0);
    std::memcpy(d.data(),"generated rate probe",20);
    d[42]=0; d[43]=64; d[45]=48; d[48]=0; d[49]=64;
    d[950]=1; d[951]=127;
    std::memcpy(d.data()+1080,"M.K.",4);
    for (int row=0;row<64;row+=8) {
        unsigned p=1084+row*16;
        int period=(row&8)?339:428;
        d[p]=static_cast<unsigned char>(period>>8);
        d[p+1]=static_cast<unsigned char>(period);
        d[p+2]=0x10;
    }
    for (int i=0;i<128;++i) d[1084+1024+i]=static_cast<unsigned char>((i%64)*3-96);
    return d;
}
#endif
