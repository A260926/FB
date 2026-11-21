#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/core/ansi_allocator.h>
#include <acl/core/track_writer.h>
#include <acl/decompression/decompress.h>
#include <acl/core/impl/compressed_headers.h>
#include <rtm/vqvf.h>
#include <rtm/quatf.h>
#include <rtm/vector4f.h>
#include <cmath>
#include <cstdio>
#include <utility>
using namespace acl;
struct pose_writer : track_writer {
    rtm::vqvf pose[9];
    void RTM_SIMD_CALL write_rotation(uint32_t j, rtm::quatf_arg0 x) { pose[j].rotation=x; }
    void RTM_SIMD_CALL write_translation(uint32_t j, rtm::vector4f_arg0 x) { pose[j].translation=x; }
    void RTM_SIMD_CALL write_scale(uint32_t j, rtm::vector4f_arg0 x) { pose[j].scale=x; }
};
int main() {
    ansi_allocator allocator;
    track_array_vqvf raw(allocator,9);
    for(uint32_t j=0;j<9;++j) {
        track_desc_transformf desc;desc.output_index=j;
        desc.parent_index=j?j-1:k_invalid_track_index;
        desc.precision=0.001F;desc.shell_distance=1.F;
        auto track=track_vqvf::make_reserve(desc,allocator,65,30.F);
        for(uint32_t s=0;s<65;++s) {
            float t=float(s)/30.F;
            auto q=j%3==0?rtm::quat_identity():rtm::quat_from_euler(.1F*t,.07F*t*float(j+1),.2F*t);
            auto p=j%3==1?rtm::vector_set(.3F,.1F,0.F):rtm::vector_set(.1F*float(j)+.05F*std::sin(t),.2F*t,0.F);
            auto v=j%3==2?rtm::vector_set(1.F+.03F*t,1.F,1.F-.01F*t):rtm::vector_set(1.F);
            track[s]=rtm::vqv_set(q,p,v);
        }
        raw[j]=std::move(track);
    }
    auto settings=get_default_compression_settings();
    settings.optimize_loops=false;
    settings.transform_runtime_type=transform_runtime_type_t::vqmf;
    compressed_tracks* packed=nullptr;output_stats stats;
    auto error=compress_track_list(allocator,raw,settings,packed,stats);
    if(error.any()||!packed||packed->is_valid(true).any())return 10;
    auto segments=acl_impl::get_transform_tracks_header(*packed).num_segments;
    if(segments<2||packed->get_num_tracks()!=9)return 11;
    decompression_context<debug_transform_decompression_settings> ctx;
    if(!ctx.initialize(*packed))return 12;
    const float times[]={.201F,1.543F,.207F,2.1333333F,.201F};
    double checksum=0;
    for(float t:times) {
        pose_writer all,one;
        for(unsigned j=0;j<9;++j)all.pose[j]=one.pose[j]=rtm::vqv_identity();
        ctx.seek(t,sample_rounding_policy::none);ctx.decompress_tracks(all);
        for(unsigned j=0;j<9;++j) {
            ctx.decompress_track(j,one);
            if(!rtm::vector_all_near_equal3(all.pose[j].translation,one.pose[j].translation,1.e-5F)||
               !rtm::vector_all_near_equal3(all.pose[j].scale,one.pose[j].scale,1.e-5F)||
               !rtm::quat_near_equal(all.pose[j].rotation,one.pose[j].rotation,1.e-5F))return 13;
            checksum+=double(rtm::vector_get_x(all.pose[j].translation));
        }
    }
    if(!std::isfinite(checksum)||checksum<10)return 14;
    std::printf("ACL_BASELINE_OK tracks=9 samples=65 segments=%u queries=5 checksum=%.8f bytes=%u\n",unsigned(segments),checksum,packed->get_size());
    allocator.deallocate(packed,packed->get_size());
    return 0;
}
