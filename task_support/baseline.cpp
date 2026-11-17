#include "aribcaption/aribcaption.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace aribcaption;
static uint16_t crc16(const std::vector<uint8_t>& packet) {
 uint16_t crc = 0xffff;
 for (size_t i = 3; i < packet.size(); ++i) {
  crc ^= uint16_t(packet[i]) << 8;
  for (int bit = 0; bit < 8; ++bit)
   crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
 }
 return crc;
}
static void append_crc(std::vector<uint8_t>& packet) {
 const auto crc = crc16(packet);
 packet.push_back(uint8_t(crc >> 8));
 packet.push_back(uint8_t(crc));
}
int main(){
 Context ctx; Decoder dec(ctx); if(!dec.Initialize(EncodingScheme::kARIB_STD_B24_UTF8))return 1;
 std::vector<uint8_t> packet={0x80,0xff,0xf0,4,0,0,0,10,0,0,0,6,0x1f,0x20,0,0,1,'A'};
 append_crc(packet);
 if(packet.size()!=20||crc16(packet)!=0)return 4;
 DecodeResult r; auto s=dec.Decode(packet.data(),packet.size(),1234,r);
 if(s!=DecodeStatus::kGotCaption||!r.caption||r.caption->text!="A"||r.caption->pts!=1234||r.caption->regions.empty())return 2;
 std::printf("NORMAL text=%s pts=%lld regions=%zu\n",r.caption->text.c_str(),(long long)r.caption->pts,r.caption->regions.size());
 // A legal first link carrying only the first five body bytes is not a complete statement.
 packet.resize(13);packet[4]=0;packet[5]=1;packet[6]=0;packet[7]=5;
 append_crc(packet);
 const size_t payload_length=(size_t(packet[6])<<8)|packet[7];
 const size_t required=3+5+payload_length+2;
 const auto residue=crc16(packet);
 if(packet[4]!=0||packet[5]!=1||payload_length!=5||packet.size()!=required||residue!=0)return 5;
 std::printf("FIRST_LINK bytes=%zu required=%zu link=%u last=%u payload=%zu crc=%02x%02x residue=%04x\n",
             packet.size(),required,unsigned(packet[4]),unsigned(packet[5]),payload_length,
             unsigned(packet[13]),unsigned(packet[14]),unsigned(residue));
 s=dec.Decode(packet.data(),packet.size(),2000,r);
 if(s!=DecodeStatus::kError||r.caption)return 3;
 std::puts("GAP first_link=ERROR (transport fields are not assembled)");return 0;
}
