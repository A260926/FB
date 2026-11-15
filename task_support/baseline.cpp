#include "basisu_transcoder.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdint>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
    if (!in || data.empty()) return 3;
    basist::basisu_transcoder_init();
    basist::basisu_transcoder decoder;
    basist::basisu_file_info info;
    if (!decoder.validate_file_checksums(data.data(), data.size(), true) ||
        !decoder.get_file_info(data.data(), data.size(), info) ||
        !decoder.start_transcoding(data.data(), data.size())) return 4;
    std::printf("BASIS_INFO images=%u type=%u format=%u alpha=%u slices=%zu\n",
        info.m_total_images, unsigned(info.m_tex_type), unsigned(info.m_tex_format),
        unsigned(info.m_has_alpha_slices), size_t(info.m_slice_info.size()));
    basist::basisu_transcoder_state refs;
    uint64_t digest=14695981039346656037ULL;
    uint64_t alpha_digest=14695981039346656037ULL;
    size_t alpha_pixels=0;
    for (uint32_t frame=0; frame<info.m_total_images; ++frame) {
        basist::basisu_image_level_info level;
        if (!decoder.get_image_level_info(data.data(),data.size(),level,frame,0)) return 5;
        std::vector<uint8_t> pixels(size_t(level.m_orig_width)*level.m_orig_height*4);
        if (!decoder.transcode_image_level(data.data(), data.size(),frame,0,pixels.data(),
                pixels.size()/4,basist::transcoder_texture_format::cTFRGBA32,0,0,&refs)) return 6;
        for (size_t i=0; i<pixels.size(); ++i) { digest^=pixels[i];digest*=1099511628211ULL; }
        for (size_t i=3; i<pixels.size(); i+=4) { alpha_digest^=pixels[i];alpha_digest*=1099511628211ULL;alpha_pixels+=pixels[i]!=255; }
        uint64_t refhash=14695981039346656037ULL;
        for(unsigned a=0;a<2;++a) for(uint32_t x:refs.m_prev_frame_indices[a][0]) {refhash^=x;refhash*=1099511628211ULL;}
        std::printf("FRAME %u %ux%u blocks=%u iframe=%u rgb_refs=%zu alpha_refs=%zu refs=%016llx\n",
            frame, level.m_orig_width,level.m_orig_height,level.m_total_blocks,
            unsigned(level.m_iframe_flag),size_t(refs.m_prev_frame_indices[0][0].size()),
            size_t(refs.m_prev_frame_indices[1][0].size()),(unsigned long long)refhash);
    }
    std::printf("BASELINE_OK frames=%u rgba_fnv64=%016llx alpha_fnv64=%016llx nonopaque=%zu\n",
        info.m_total_images,(unsigned long long)digest,(unsigned long long)alpha_digest,alpha_pixels);
    return 0;
}
