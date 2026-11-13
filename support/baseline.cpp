#include <acl/compression/compress.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/impl/debug_track_writer.h>
#include <acl/decompression/decompress.h>
#include <rtm/vqvf.h>
#include <cmath>
#include <cstdio>
int main(){
 using namespace acl;
 ansi_allocator a; constexpr unsigned N=81,B=8; track_array_vqvf raw(a,B);
 for(unsigned b=0;b<B;++b){
  track_desc_transformf d;d.output_index=b;d.parent_index=b?b-1:k_invalid_track_index;d.precision=.0001f;
  auto t=track_vqvf::make_reserve(d,a,N,30.f);
  for(unsigned f=0;f<N;++f){float x=float(f)*.02f + .05f*std::sin(float(f)*.3f+b);
   t[f]=rtm::vqv_set(rtm::quat_identity(),rtm::vector_set(x,float(b)*.1f,0.f),rtm::vector_set(1.f));}
  raw[b]=std::move(t);
 }
 vqvf_transform_error_metric metric;auto s=get_default_compression_settings();s.error_metric=&metric;s.optimize_loops=false;
 compressed_tracks *plain=nullptr,*stripped=nullptr;output_stats stats;
 auto r=compress_track_list(a,raw,s,plain,stats);if(r.any()){std::puts(r.c_str());return 1;}
 s.keyframe_stripping.proportion=.8f;
 r=compress_track_list(a,raw,s,stripped,stats);if(r.any()){std::puts(r.c_str());return 2;}
 decompression_context<default_transform_decompression_settings> ctx;
 if(!ctx.initialize(*stripped))return 3;
 acl_impl::debug_track_writer writer(a,track_type8::vqvf,B);ctx.seek(0.f,sample_rounding_policy::none);ctx.decompress_tracks(writer);
 float x=rtm::vector_get_x(writer.read_vqv(0).translation);
 if(!std::isfinite(x)||std::fabs(x)>1e-3f||stripped->get_size()>=plain->get_size())return 4;
 std::printf("ACL_BASELINE_OK tracks=%u frames=%u plain=%u stripped=%u root0=%.6f\n",B,N,plain->get_size(),stripped->get_size(),x);
 a.deallocate(plain,plain->get_size());a.deallocate(stripped,stripped->get_size());
}
