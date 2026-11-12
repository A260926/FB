#include "ymfm_opn.cpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
    ymfm::ymfm_interface io;
    ymfm::fm_engine_base<ymfm::opna_registers> fm(io);
    fm.reset();
    fm.write(0xb0,7); fm.write(0xb4,0xc0);
    for(unsigned s=0;s<4;s++) {
        unsigned off=s*4;
        fm.write(0x30+off,1);fm.write(0x40+off,0x20);
        fm.write(0x50+off,0x1f);fm.write(0x60+off,0x0c);
        fm.write(0x70+off,0x08);fm.write(0x80+off,0x4f);
    }
    fm.write(0xa4,0x22);fm.write(0xa0,0x69);fm.write(0x28,0xf0);
    uint64_t energy=0;
    for(unsigned n=0;n<4096;n++) {
        fm.clock(1);
        ymfm::ymfm_output<2> out;out.clear();fm.output(out,0,32767,1);
        energy+=uint64_t(int64_t(out.data[0])*out.data[0]);
    }
    assert(energy>0);
    assert(fm.debug_operator(0)->debug_eg_state()!=ymfm::EG_ATTACK);
    std::puts("BASELINE_OPNA_AUDIO_OK frames=4096");
}
