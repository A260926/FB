## RtspServer
A RTSP server and pusher based on C++11

[中文介绍](https://github.com/PHZ76/RtspServer/blob/master/README_CN.md)

## Features
 - Support Linux and Windows platforms.
 - Support H.264/H.265/G711A/AAC
 - Support unicast(rtp over udp, rtp over rtsp), multicast
 - Support digest authentication
 - Support RFC 4585 Generic NACK retransmission (unicast only)

## Generic NACK retransmission
 - Scope: RFC 4585 RTPFB/FMT=1 (Generic NACK) only. No RTX payload, no
   separate retransmission SSRC, no SDP negotiation; multicast and pusher
   behavior is unchanged.
 - The server keeps the last 512 RTP packets actually sent for every media
   channel of every connection. A NACK must arrive on the negotiated RTCP
   channel (TCP interleaved) or RTCP port (UDP) of that media and carry the
   matching media SSRC; unknown packets are ignored.
 - Retransmitted RTP bytes are identical to the first send and do not consume
   sequence numbers; over TCP they use the media's RTP interleaved channel.
 - Example: example/rtsp_nack_retransmit.cpp (self-contained, no external
   player needed).

## Requirements
- Compiler support c++11, gcc4.8/vs2015 or above.

## Example
 - [rtsp-server](https://github.com/PHZ76/RtspServer/blob/master/example/rtsp_h264_file.cpp)
 - [DesktopSharing](https://github.com/PHZ76/DesktopSharing)

## Dependencies
 - [websocketpp-md5](https://github.com/zaphoyd/websocketpp)

## License
 - [MIT License](https://github.com/PHZ76/RtspServer/blob/master/LICENSE)

## Contact
 - Email: penghaoze76@qq.com
