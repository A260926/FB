#ifndef TASK_PES_HPP
#define TASK_PES_HPP
#include <cstdint>
#include <vector>
#include <initializer_list>
namespace task_support {
inline void be24(std::vector<uint8_t>& out, size_t n) {
 out.push_back((n>>16)&255);out.push_back((n>>8)&255);out.push_back(n&255);
}
inline std::vector<uint8_t> pes(const std::vector<std::vector<uint8_t>>& bodies) {
 std::vector<uint8_t> units;
 for(const auto& b:bodies){units.push_back(0x1f);units.push_back(0x20);be24(units,b.size());units.insert(units.end(),b.begin(),b.end());}
 std::vector<uint8_t> payload{0};be24(payload,units.size());payload.insert(payload.end(),units.begin(),units.end());
 std::vector<uint8_t> out{0x80,0xff,0x00,0x04,0x00,0x00,uint8_t(payload.size()>>8),uint8_t(payload.size())};
 out.insert(out.end(),payload.begin(),payload.end());return out;
}
}
#endif
