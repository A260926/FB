#include <charls/charls.hpp>
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

TEST(start_baseline, full_frame_old_paths) {
    for (const auto mode : {charls::interleave_mode::none, charls::interleave_mode::line, charls::interleave_mode::sample}) {
        const int components = mode == charls::interleave_mode::none ? 1 : 3;
        charls::frame_info frame{37, 19, 8, components};
        std::vector<std::byte> raw(size_t(frame.width)*frame.height*components);
        for (size_t i=0; i<raw.size(); ++i) raw[i]=std::byte((i*73+(i/37)*11)%256);
        auto compressed=charls::jpegls_encoder::encode<std::vector<std::byte>>(raw,frame,mode);
        charls::jpegls_decoder decoder{compressed,true};
        std::vector<std::byte> actual(decoder.get_destination_size());
        decoder.decode(actual);
        ASSERT_EQ(raw,actual);
        std::cout << "FULL_FRAME_OLD_PATH_OK mode=" << int(mode) << " bytes=" << actual.size() << '\n';
    }
}
TEST(start_baseline, high_precision_near_lossless) {
    charls::frame_info frame{31,17,12,1};
    std::vector<uint16_t> raw(size_t(frame.width)*frame.height);
    for(size_t i=0;i<raw.size();++i)raw[i]=uint16_t((i*113+i/31)%4096);
    charls::jpegls_encoder enc;
    enc.frame_info(frame).near_lossless(3);
    std::vector<std::byte> compressed(enc.estimated_destination_size());
    enc.destination(compressed);compressed.resize(enc.encode(raw));
    charls::jpegls_decoder dec{compressed,true};
    std::vector<uint16_t> actual(raw.size());dec.decode(actual);
    for(size_t i=0;i<raw.size();++i)ASSERT_LE(std::abs(int(raw[i])-int(actual[i])),3);
    std::cout << "HIGH_PRECISION_OLD_PATH_OK samples=" << actual.size() << '\n';
}
