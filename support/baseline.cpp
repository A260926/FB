#include <cstdio>
#include <cmath>
#define TSF_IMPLEMENTATION
#include "tsf.h"
int main(){
 tsf* f=tsf_load_filename("examples/florestan-subset.sf2");if(!f)return 1;
 if(tsf_get_presetcount(f)<1)return 2;
 tsf_set_output(f,TSF_STEREO_INTERLEAVED,44100,0);
 tsf* g=tsf_copy(f);if(!g)return 3;
 if(!tsf_note_on(f,0,60,0.7f)||!tsf_note_on(g,0,60,0.7f))return 4;
 float a[2048],b[2048];tsf_render_float(f,a,1024,0);tsf_render_float(g,b,1024,0);
 double energy=0;for(int i=0;i<2048;++i){if(!std::isfinite(a[i])||a[i]!=b[i])return 5;energy+=a[i]*a[i];}
 if(energy<1e-8)return 6;
 tsf_note_off(f,0,60);if(tsf_active_voice_count(g)<1)return 7;
 tsf_close(f);tsf_render_float(g,b,1024,0);tsf_close(g);
 std::printf("BASELINE_OK presets_rendered=1 frames=1024 copy_equal=1 energy=%.6f\n",energy);
}
