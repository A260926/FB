#ifndef POOL_UNDO_FIXTURE_H
#define POOL_UNDO_FIXTURE_H
#include "common/internal.h"
#include "core/box.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static lsmash_root_t *open_movie(const char *name, lsmash_file_parameters_t *file, uint32_t *track) {
 lsmash_root_t *root=lsmash_create_root();
 if(!root || lsmash_open_file(name,0,file)<0)return NULL;
 static lsmash_brand_type brands[]={ISOM_BRAND_TYPE_ISOM,ISOM_BRAND_TYPE_ISO6,ISOM_BRAND_TYPE_AVC1};
 file->major_brand=ISOM_BRAND_TYPE_ISOM;file->brands=brands;file->brand_count=3;
 file->max_chunk_duration=10;file->max_async_tolerance=20;
 if(!lsmash_set_file(root,file))return NULL;
 lsmash_movie_parameters_t mp;lsmash_initialize_movie_parameters(&mp);mp.timescale=1000;
 if(lsmash_set_movie_parameters(root,&mp)<0)return NULL;
 *track=lsmash_create_track(root,ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK);
 lsmash_track_parameters_t tp;lsmash_initialize_track_parameters(&tp);
 tp.display_width=16<<16;tp.display_height=16<<16;
 if(!*track || lsmash_set_track_parameters(root,*track,&tp)<0)return NULL;
 lsmash_media_parameters_t media;lsmash_initialize_media_parameters(&media);media.timescale=1000;
 if(lsmash_set_media_parameters(root,*track,&media)<0)return NULL;
 lsmash_video_summary_t *v=(lsmash_video_summary_t*)lsmash_create_summary(LSMASH_SUMMARY_TYPE_VIDEO);
 if(!v)return NULL;
 v->sample_type=ISOM_CODEC_TYPE_AVC1_VIDEO;v->width=16;v->height=16;
 lsmash_codec_specific_t *c=lsmash_create_codec_specific_data(LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_H264,LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED);
 if(!c)return NULL;
 lsmash_h264_specific_parameters_t *avc=c->data.structured;avc->lengthSizeMinusOne=3;
 const uint8_t sps[]={0x67,0x42,0xc0,0x0a,0xda,0x7b,0x01,0x10,0,0,3,0,0x10,0,0,3,3,0xc0,0xf1,0x22,0x6a};
 const uint8_t pps[]={0x68,0xce,0x0f,0xc8};
 if(lsmash_append_h264_parameter_set(avc,H264_PARAMETER_SET_TYPE_SPS,(void*)sps,sizeof(sps))<0 || lsmash_append_h264_parameter_set(avc,H264_PARAMETER_SET_TYPE_PPS,(void*)pps,sizeof(pps))<0)return NULL;
 if(lsmash_add_codec_specific_data((lsmash_summary_t*)v,c)<0)return NULL;
 lsmash_destroy_codec_specific_data(c);
 int index=lsmash_add_sample_entry(root,*track,(lsmash_summary_t*)v);
 lsmash_cleanup_summary((lsmash_summary_t*)v);
 if(index!=1)return NULL;
 return root;
}
static int append_packet(lsmash_root_t*root,uint32_t track,uint64_t dts,uint64_t cts,uint8_t marker) {
 lsmash_sample_t*s=lsmash_create_sample(8);if(!s)return -1;
 const uint8_t data[]={0,0,0,4,0x65,0x88,0x84,0};memcpy(s->data,data,8);s->data[7]=marker;
 s->dts=dts;s->cts=cts;s->index=1;s->prop.ra_flags=ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
 int e=lsmash_append_sample(root,track,s);if(e<0)lsmash_delete_sample(s);return e;
}
static int close_movie(lsmash_root_t*root,uint32_t track,lsmash_file_parameters_t*file){
 if(lsmash_flush_pooled_samples(root,track,40)<0 || lsmash_finish_movie(root,NULL)<0)return -1;
 if(lsmash_close_file(file)<0)return -1;lsmash_destroy_root(root);return 0;
}
static int verify_movie(const char*name,uint32_t expected,const uint8_t*markers){
 lsmash_root_t*r=lsmash_create_root();lsmash_file_parameters_t f={0};
 if(!r || lsmash_open_file(name,1,&f)<0)return -1;
 lsmash_file_t*file=lsmash_set_file(r,&f);
 if(!file || lsmash_read_file(file,&f)<0)return -1;
 uint32_t t=lsmash_get_track_ID(r,1);
 if(!t || lsmash_construct_timeline(r,t)<0 || lsmash_get_sample_count_in_media_timeline(r,t)!=expected)return -1;
 for(uint32_t i=1;i<=expected;++i){
  lsmash_sample_t*s=lsmash_get_sample_from_media_timeline(r,t,i);
  if(!s || s->length!=8 || s->data[7]!=markers[i-1] || s->dts!=(uint64_t)(i-1)*40)return -1;
  lsmash_delete_sample(s);
 }
 lsmash_close_file(&f);lsmash_destroy_root(r);return 0;
}
#endif
