#include <acl/compression/compress.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/track_writer.h>
#include <acl/decompression/decompress.h>
#include <acl/decompression/database/impl/debug_database_streamer.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
using namespace acl;
#define need(x) do { if(!(x)) { std::fprintf(stderr,"baseline assertion line %d: %s\n",__LINE__,#x); return 2; } } while(0)
struct settings_db : default_transform_decompression_settings { using database_settings_type=debug_database_settings; };
struct writer : track_writer {
 rtm::vqvf pose[2];
 void RTM_SIMD_CALL write_rotation(uint32_t i,rtm::quatf_arg0 v){pose[i].rotation=v;}
 void RTM_SIMD_CALL write_translation(uint32_t i,rtm::vector4f_arg0 v){pose[i].translation=v;}
 void RTM_SIMD_CALL write_scale(uint32_t i,rtm::vector4f_arg0 v){pose[i].scale=v;}
};
int main(){
 ansi_allocator a;
 track_array_vqvf tracks(a,2);
 for(uint32_t b=0;b<2;++b){
  track_desc_transformf desc;desc.output_index=b;desc.parent_index=b?0:k_invalid_track_index;desc.precision=0.00001f;
  auto t=track_vqvf::make_reserve(desc,a,64,30.0f);
  for(uint32_t f=0;f<64;++f){float x=float(f)*0.2f; t[f]=rtm::vqv_set(rtm::quat_identity(),rtm::vector_set(std::sin(x*(b+1))*3,float(b),std::cos(x)*2),rtm::vector_set(1.0f));}
  tracks[b]=std::move(t);
 }
 auto cs=get_default_compression_settings();qvvf_transform_error_metric metric;cs.error_metric=&metric;cs.enable_database_support=true;cs.keyframe_stripping=compression_keyframe_stripping_settings{};
 compressed_tracks* compressed=nullptr;output_stats stats;auto ce=compress_track_list(a,tracks,cs,compressed,stats);if(!ce.empty())std::fprintf(stderr,"compress: %s\n",ce.c_str());need(ce.empty());
 compression_database_settings ds;ds.medium_importance_tier_proportion=.4f;ds.low_importance_tier_proportion=.4f;ds.max_chunk_size=4096;
 compressed_tracks* dbtracks[1]={nullptr};compressed_database* db=nullptr;const compressed_tracks* inputs[]={compressed};
 need(build_database(a,ds,inputs,1,dbtracks,db).empty());
 compressed_database* split=nullptr;uint8_t *mid=nullptr,*low=nullptr;need(split_database_bulk_data(a,*db,split,mid,low).empty());
 need(split->get_num_chunks(quality_tier::medium_importance)>0&&split->get_num_chunks(quality_tier::lowest_importance)>0);
 {
 debug_database_streamer sm(a,mid,split->get_bulk_data_size(quality_tier::medium_importance));
 debug_database_streamer sl(a,low,split->get_bulk_data_size(quality_tier::lowest_importance));
 database_context<debug_database_settings> dc;need(dc.initialize(a,*split,sm,sl));
 decompression_context<settings_db> dec;need(dec.initialize(*dbtracks[0],dc));
 need(dc.stream_in(quality_tier::medium_importance)==database_stream_request_result::dispatched);
 need(dc.stream_in(quality_tier::lowest_importance)==database_stream_request_result::dispatched);
 need(dc.is_streamed_in(quality_tier::lowest_importance));
 float full[64];float maxerr=0;
 for(unsigned f=0;f<64;++f){writer w;dec.seek(float(f)/30,sample_rounding_policy::none);dec.decompress_tracks(w);full[f]=rtm::vector_get_x(w.pose[0].translation);maxerr=std::fmax(maxerr,std::fabs(full[f]-std::sin(float(f)*.2f)*3));}
 need(maxerr<.001f);
 need(dc.stream_out(quality_tier::lowest_importance)==database_stream_request_result::dispatched);
 need(!dc.is_streamed_in(quality_tier::lowest_importance));float delta=0;
 for(unsigned f=0;f<64;++f){writer w;dec.seek(float(f)/30,sample_rounding_policy::none);dec.decompress_tracks(w);delta=std::fmax(delta,std::fabs(full[f]-rtm::vector_get_x(w.pose[0].translation)));}
 need(delta>1e-5f);
 std::printf("ACL_BASELINE_OK frames=64 bones=2 full_error=%.8f evicted_delta=%.8f medium_chunks=%u low_chunks=%u\n",maxerr,delta,split->get_num_chunks(quality_tier::medium_importance),split->get_num_chunks(quality_tier::lowest_importance));
 }
 a.deallocate(mid,split->get_bulk_data_size(quality_tier::medium_importance));a.deallocate(low,split->get_bulk_data_size(quality_tier::lowest_importance));
 a.deallocate(split,split->get_size());a.deallocate(dbtracks[0],dbtracks[0]->get_size());a.deallocate(db,db->get_size());a.deallocate(compressed,compressed->get_size());
}
