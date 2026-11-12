#include <fluidlite.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
  fluid_settings_t *settings=new_fluid_settings();
  fluid_synth_t *synth;
  fluid_sfont_t *sf=fluid_ramsfont_create_sfont();
  fluid_sample_t *sample=new_fluid_ramsample();
  short data[256]; float left[4096],right[4096];
  int i,id; double energy=0;
  assert(settings&&sf&&sample);
  assert(fluid_settings_setnum(settings,"synth.sample-rate",44100));
  synth=new_fluid_synth(settings);assert(synth);
  fluid_synth_set_reverb_on(synth,0);fluid_synth_set_chorus_on(synth,0);
  for(i=0;i<256;i++) data[i]=(short)(12000*sin(6.283185307179586*i/256));
  assert(fluid_sample_set_sound_data(sample,data,256,1,60)==0);
  assert(fluid_ramsfont_add_izone(sf->data,0,0,sample,0,127)==0);
  assert(fluid_ramsfont_izone_set_loop(sf->data,0,0,sample,1,0,0)==0);
  id=fluid_synth_add_sfont(synth,sf);assert(id>=0);
  assert(fluid_synth_program_select(synth,0,id,0,0)==0);
  assert(fluid_synth_noteon(synth,0,60,100)==0);
  assert(fluid_synth_write_float(synth,4096,left,0,1,right,0,1)==0);
  for(i=0;i<4096;i++) {assert(isfinite(left[i])&&isfinite(right[i]));energy+=left[i]*left[i]+right[i]*right[i];}
  assert(energy>0.01);
  assert(fluid_synth_noteoff(synth,0,60)==0);
  delete_fluid_synth(synth);delete_fluid_settings(settings);
  /* RAM SoundFont owns sample and releases it with the synth. */
  printf("FLUIDLITE_BASELINE_OK frames=4096 ram-sample=1\n");
  return 0;
}
