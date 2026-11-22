#include "src/rtp.hh"
#include "src/formats/h264.hh"
#include "uvgrtp/frame.hh"
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#define MUST(x) do { if (!(x)) { std::cerr << "baseline assertion line " << __LINE__ << "\n"; return 1; } } while (0)
static std::vector<uint8_t> packet(uint16_t seq, uint32_t ts, std::initializer_list<uint8_t> payload) {
    std::vector<uint8_t> b(12,0); b[0]=0x80; b[1]=96;
    b[2]=uint8_t(seq>>8); b[3]=uint8_t(seq);
    for(int i=0;i<4;++i) b[4+i]=uint8_t(ts>>(24-8*i));
    b[11]=7; b.insert(b.end(),payload);return b;
}
int main() {
    auto ssrc=std::make_shared<std::atomic<uint32_t>>(7);
    auto rtp=std::make_shared<uvgrtp::rtp>(RTP_FORMAT_H264,ssrc,false);
    std::shared_ptr<uvgrtp::socket> no_socket;
    const int flags=RCE_NO_H26X_PREPEND_SC;
    uvgrtp::formats::h264 media(no_socket,rtp,flags);
    auto feed=[&](std::vector<uint8_t>& bytes,uvgrtp::frame::rtp_frame** out) {
        auto status=rtp->packet_handler(nullptr,flags,bytes.data(),bytes.size(),out);
        if(status!=RTP_PKT_MODIFIED)return status;
        return media.packet_handler(nullptr,flags,bytes.data(),bytes.size(),out);
    };
    uvgrtp::frame::rtp_frame* out=nullptr;
    auto start=packet(100,9000,{0x7c,0x85,'A','B'});
    auto end=packet(101,9000,{0x7c,0x45,'C','D'});
    MUST(feed(start,&out)==RTP_OK);
    MUST(feed(end,&out)==RTP_PKT_READY);
    const uint8_t expected[]={0x65,'A','B','C','D'};
    MUST(out && out->payload_len==sizeof expected);
    MUST(std::memcmp(out->payload,expected,sizeof expected)==0);
    MUST(out->header.timestamp==9000 && out->header.ssrc==7);
    MUST(uvgrtp::frame::dealloc_frame(out)==RTP_OK);out=nullptr;
    auto jump=packet(20000,10,{0x65,'N'});
    MUST(feed(jump,&out)==RTP_PKT_READY);
    MUST(out && out->header.seq==20000 && out->payload_len==2 && out->payload[1]=='N');
    MUST(uvgrtp::frame::dealloc_frame(out)==RTP_OK);out=nullptr;
    std::cout << "UVGRTP_RECEIVE_BASELINE_OK fu_payload=5 jump_still_delivered=1\n";
    return 0;
}
