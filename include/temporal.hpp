#pragma once
#include "frame.hpp"
namespace render {
struct TemporalSettings {
 double history_weight=0.9;
 double depth_relative_threshold=0.01;
};
class TemporalAccumulator {
 Image history_color_;
 std::vector<double> history_depth_;
 std::vector<uint32_t> history_object_;
 V2 history_jitter_{};
 double history_exposure_=1;
 uint64_t history_sequence_=0;
 bool has_history_=false;
public:
 Image resolve(const Frame& frame,const TemporalSettings& settings=TemporalSettings{});
 void reset();
};
}
