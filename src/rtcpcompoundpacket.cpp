/*

  This file is a part of JRTPLIB
  Copyright (c) 1999-2004 Jori Liesenborgs

  Contact: jori@lumumba.luc.ac.be

  This library was developed at the "Expertisecentrum Digitale Media"
  (http://www.edm.luc.ac.be), a research center of the "Limburgs Universitair
  Centrum" (http://www.luc.ac.be). The library is based upon work done for 
  my thesis at the School for Knowledge Technology (Belgium/The Netherlands).

  Permission is hereby granted, free of charge, to any person obtaining a
  copy of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom the
  Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include "rtcpcompoundpacket.h"
#include "rtprawpacket.h"
#include "rtperrors.h"
#include "rtpstructs.h"
#include "rtpdefines.h"
#include "rtcpsrpacket.h"
#include "rtcprrpacket.h"
#include "rtcpsdespacket.h"
#include "rtcpbyepacket.h"
#include "rtcpapppacket.h"
#include "rtcpunknownpacket.h"
#ifndef WIN32
	#include <netinet/in.h>
#else
	#include <winsock2.h>
#endif // WIN32

#include "rtpdebug.h"

RTCPCompoundPacket::RTCPCompoundPacket(RTPRawPacket &rawpack)
{
	compoundpacket = 0;
	compoundpacketlength = 0;
	error = 0;
	
	if (rawpack.IsRTP())
	{
		error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
		return;
	}

	u_int8_t *data = rawpack.GetData();
	size_t datalen = rawpack.GetDataLength();
	bool first;
	
	if (datalen < sizeof(RTCPCommonHeader))
	{
		error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
		return;
	}

	first = true;
	
	do
	{
		RTCPCommonHeader *rtcphdr;
		size_t length;
		
		rtcphdr = (RTCPCommonHeader *)data;
		if (rtcphdr->version != RTP_VERSION) // check version
		{
			ClearPacketList();
			error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
			return;
		}
		if (first)
		{
			// Check if first packet is SR or RR
			
			first = false;
			if ( ! (rtcphdr->packettype == RTP_RTCPTYPE_SR || rtcphdr->packettype == RTP_RTCPTYPE_RR))
			{
				ClearPacketList();
				error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
				return;
			}
		}
		
		length = (size_t)ntohs(rtcphdr->length);
		length++;
		length *= sizeof(u_int32_t);

		if (length > datalen) // invalid length field
		{
			ClearPacketList();
			error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
			return;
		}
		
		if (rtcphdr->padding)
		{
			// check if it's the last packet
			if (length != datalen)
			{
				ClearPacketList();
				error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
				return; // not last packet
			}
		}

		RTCPPacket *p;
		
		switch (rtcphdr->packettype)
		{
		case RTP_RTCPTYPE_SR:
			p = new RTCPSRPacket(data,length);
			break;
		case RTP_RTCPTYPE_RR:
			p = new RTCPRRPacket(data,length);
			break;
		case RTP_RTCPTYPE_SDES:
			p = new RTCPSDESPacket(data,length);
			break;
		case RTP_RTCPTYPE_BYE:
			p = new RTCPBYEPacket(data,length);
			break;
		case RTP_RTCPTYPE_APP:
			p = new RTCPAPPPacket(data,length);
			break;
		default:
			p = new RTCPUnknownPacket(data,length);
		}

		if (p == 0)
		{
			ClearPacketList();
			error = ERR_RTP_OUTOFMEM;
			return;
		}

		rtcppacklist.push_back(p);
		
		datalen -= length;
		data += length;
	} while (datalen >= (size_t)sizeof(RTCPCommonHeader));

	if (datalen != 0) // some remaining bytes
	{
		ClearPacketList();
		error = ERR_RTP_RTCPCOMPOUND_INVALIDPACKET;
		return;
	}

	compoundpacket = rawpack.GetData();
	compoundpacketlength = rawpack.GetDataLength();

	rawpack.ZeroData();
	
	rtcppackit = rtcppacklist.begin();
}

RTCPCompoundPacket::RTCPCompoundPacket()
{
	compoundpacket = 0;
	compoundpacketlength = 0;
	error = 0;
}

RTCPCompoundPacket::~RTCPCompoundPacket()
{
	ClearPacketList();
	if (compoundpacket)
		delete [] compoundpacket;
}

static void AppendNACKSequence(std::vector<uint16_t>& out,
                               std::vector<unsigned char>& seen,
                               uint16_t sequence)
{
	if (!seen[sequence])
	{
		seen[sequence] = 1;
		out.push_back(sequence);
	}
}

int RTCPCompoundPacket::GetGenericNACKs(uint32_t mediaSSRC,
                                        std::vector<uint16_t>& out) const
{
	if (error < 0 || compoundpacket == 0)
		return -1;

	std::vector<uint16_t> result;
	std::vector<unsigned char> seen(65536, 0);
	const size_t totalLength = compoundpacketlength;
	size_t offset = 0;

	// Walk the raw compound so that length and padding can be verified
	// independently of the parsed RTCPPacket wrappers.
	while (offset < totalLength)
	{
		if (totalLength - offset < 4)
			return -1;

		const u_int8_t *packet = compoundpacket + offset;
		unsigned char firstByte = packet[0];
		bool padding = (firstByte & 0x20) != 0;
		unsigned format = firstByte & 0x1f;
		unsigned packetType = packet[1];
		size_t lengthWords = (static_cast<size_t>(packet[2]) << 8) |
		                     static_cast<size_t>(packet[3]);
		size_t packetLength = (lengthWords + 1) * sizeof(uint32_t);

		if ((firstByte >> 6) != RTP_VERSION)
			return -1;
		if (offset + packetLength > totalLength)
			return -1;

		if (packetType == 205 && format == 1) // RTPFB, Generic NACK
		{
			// Common RTCP header (4) + sender SSRC (4) + media SSRC (4),
			// followed by at least one four-byte PID/BLP FCI entry.
			if (packetLength < 16)
				return -1;

			size_t paddingLength = 0;
			if (padding)
			{
				// Padding is only valid on the final compound subpacket.
				if (offset + packetLength != totalLength)
					return -1;

				paddingLength = packet[packetLength - 1];
				if (paddingLength == 0 || paddingLength > packetLength - 16)
					return -1;
			}

			size_t fciLength = packetLength - 12 - paddingLength;
			if (fciLength < 4 || (fciLength % 4) != 0)
				return -1;

			uint32_t packetMediaSSRC =
				(static_cast<uint32_t>(packet[8]) << 24) |
				(static_cast<uint32_t>(packet[9]) << 16) |
				(static_cast<uint32_t>(packet[10]) << 8) |
				static_cast<uint32_t>(packet[11]);

			// Validate every NACK, even one addressed to another SSRC.
			if (packetMediaSSRC == mediaSSRC)
			{
				for (size_t fciOffset = 12;
				     fciOffset < 12 + fciLength;
				     fciOffset += 4)
				{
					uint16_t pid = static_cast<uint16_t>(
						(static_cast<uint16_t>(packet[fciOffset]) << 8) |
						static_cast<uint16_t>(packet[fciOffset + 1]));
					uint16_t blp = static_cast<uint16_t>(
						(static_cast<uint16_t>(packet[fciOffset + 2]) << 8) |
						static_cast<uint16_t>(packet[fciOffset + 3]));

					AppendNACKSequence(result, seen, pid);
					for (unsigned bit = 0; bit < 16; ++bit)
					{
						if (blp & (static_cast<unsigned>(1) << bit))
						{
							uint16_t lost = static_cast<uint16_t>(pid + bit + 1);
							AppendNACKSequence(result, seen, lost);
						}
					}
				}
			}
		}

		offset += packetLength;
	}

	if (offset != totalLength)
		return -1;

	out.swap(result);
	return 0;
}

void RTCPCompoundPacket::ClearPacketList()
{
	std::list<RTCPPacket *>::const_iterator it;

	for (it = rtcppacklist.begin() ; it != rtcppacklist.end() ; it++)
		delete *it;

	rtcppacklist.clear();
	rtcppackit = rtcppacklist.begin();
}

#ifdef RTPDEBUG
void RTCPCompoundPacket::Dump()
{
	std::list<RTCPPacket *>::const_iterator it;
	for (it = rtcppacklist.begin() ; it != rtcppacklist.end() ; it++)
	{
		RTCPPacket *p = *it;

		p->Dump();
	}
}
#endif // RTPDEBUG
