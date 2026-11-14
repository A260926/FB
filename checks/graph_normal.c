#include <stdio.h>
#include <assert.h>
#ifndef CHECK_SPLIT
#include "../miniaudio.h"
#else
#include "../extras/miniaudio_split/miniaudio.h"
#endif
#include <math.h>
#include <string.h>
static void pass(ma_node *n,const float **in,ma_uint32 *ni,float **out,ma_uint32 *no){(void)n;(void)in;(void)ni;(void)out;(void)no;}
static ma_node_vtable vt={pass,NULL,1,1,MA_NODE_FLAG_PASSTHROUGH};
int main(void){
 float input[64],out[256];for(int i=0;i<64;i++)input[i]=(float)(i+1)/64;
 ma_node_graph g;ma_node_graph_config gc=ma_node_graph_config_init(1);gc.processingSizeInFrames=8;assert(ma_node_graph_init(&gc,NULL,&g)==MA_SUCCESS);
 ma_audio_buffer_ref ref;assert(ma_audio_buffer_ref_init(ma_format_f32,1,input,64,&ref)==MA_SUCCESS);
 ma_data_source_node source;ma_data_source_node_config sc=ma_data_source_node_config_init(&ref);assert(ma_data_source_node_init(&g,&sc,NULL,&source)==MA_SUCCESS);
 ma_splitter_node split;ma_splitter_node_config sp=ma_splitter_node_config_init(1);assert(ma_splitter_node_init(&g,&sp,NULL,&split)==MA_SUCCESS);
 ma_node_base branch;ma_uint32 channels=1;ma_node_config bc=ma_node_config_init();bc.vtable=&vt;bc.pInputChannels=&channels;bc.pOutputChannels=&channels;assert(ma_node_init(&g,&bc,NULL,&branch)==MA_SUCCESS);
 assert(ma_node_attach_output_bus(&source,0,&split,0)==MA_SUCCESS);
 assert(ma_node_attach_output_bus(&split,0,ma_node_graph_get_endpoint(&g),0)==MA_SUCCESS);
 assert(ma_node_attach_output_bus(&split,1,&branch,0)==MA_SUCCESS);
 assert(ma_node_attach_output_bus(&branch,0,ma_node_graph_get_endpoint(&g),0)==MA_SUCCESS);
 assert(ma_node_set_output_bus_volume(&split,0,0.25f)==MA_SUCCESS);
 assert(ma_node_set_output_bus_volume(&branch,0,0.75f)==MA_SUCCESS);
 ma_uint64 nr=0;ma_result r=ma_node_graph_read_pcm_frames(&g,out,16,&nr);assert(r==MA_SUCCESS&&nr==16);
 for(unsigned i=0;i<16;i++)assert(fabsf(out[i]-input[i])<1e-7f);
 puts("GRAPH_BASELINE_OK frames=16");
 ma_node_uninit(&branch,NULL);ma_splitter_node_uninit(&split,NULL);ma_data_source_node_uninit(&source,NULL);ma_audio_buffer_ref_uninit(&ref);ma_node_graph_uninit(&g,NULL);return 0;
}
