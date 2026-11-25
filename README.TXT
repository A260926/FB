------------------------------------------------------------------------------
                                                               August 18, 2004


                                   JRTPLIB (v3.0.0)


                      Developed at the The Expertise Centre for 
                    Digital Media (EDM), a research institute of 
                       the Limburgs Universitair Centrum (LUC)

                              http://www.edm.luc.ac.be/
                                http://www.luc.ac.be/


LIBRARY LOCATION AND CONTACT
============================
Normally, you should be able to download the latest version of the library
from this url:
	http://research.edm.luc.ac.be/jori/jrtplib/jrtplib.html

If you have questions about the library, you can mail me at:
	jori@lumumba.luc.ac.be

ACKNOWLEDGMENT
==============
I would like thank the people at the Expertise Centre for Digital Media
for giving me the opportunity to create this rewrite of the library.

DISCLAIMER & COPYRIGHT
======================

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

INSTALLATION NOTES
==================

* To compile the library on a unix-like platform, just start the configure
  script. This should generate a makefile which you can use to start
  building the library using 'make'.

* To compile the library on an MS-Windows platfrom: an Visual Studio
  workspace file jrtplib.dsw is included. The project will search
  for JThread files in ..\jthread-1.0.1
  To compile the library without using JThread, comment the line
  with RTP_SUPPORT_THREAD in rtpconfig_win.h

* For a manual about the library, please refer to doc/ subdirectory.
  Using 'make doc', a PDF version of the documentation will be built.
  Alternatively, you can download the documentation from the homepage.

* For systems with low memory or for applications which will involve only
  a few participants at a time:
  You can set the HASHSIZE defines in rtpsources.h, rtpudpv4transmitter.h
  and rtpudpv6transmitter.h to a lower value to avoid memory being wasted.
  Note that the library will have to be recompiled.

* Used defines:
	- WIN32:
		For compilation on an MS-Windows platform.
	- RTP_HAVE_SYS_FILIO:
		Set if <sys/filio.h> exists.
	- RTP_HAVE_SYS_SOCKIO:
		Set if <sys/sockio.h> exists.
	- RTP_BIG_ENDIAN:
		If set, assume big-endian byte ordering.
	- RTP_SOCKLENTYPE_UINT:
		Indicates that getsockname used an unsigned int as its
		third parameter.
	- RTP_HAVE_SOCKADDR_LEN:
		Indicates that struct sockaddr has an sa_len field.
	- RTP_SUPPORT_IPV4MULTICAST:
		Enables support for IPv4 multicasting.
	- RTP_SUPPORT_THREAD:
		Enables support for JThread.
	- RTP_SUPPORT_SDESPRIV:
		Enables support for RTCP SDES private items.
	- RTP_SUPPORT_INLINETEMPLATEPARAM:
		If set, the compiler should be able to handle an inline
		function as a template parameter.
	- RTP_SUPPORT_PROBATION:
		If set, a few consecutive RTP packets are needed to validate
		a member.
	- RTP_SUPPORT_GNUDRAND:
		If set, the RTPRandom class will use drand48_r and srand48_r
	- RTP_SUPPORT_RANDR:
		If set and RTP_SUPPORT_GNUDRAND is not set, the RTPRandom
		class will use rand_r.
	- RTP_SUPPORT_GETLOGINR:
		If set, the library will use getlogin_r instead of getlogin.
	- RTP_SUPPORT_IPV6:
		If set, IPv6 support is enabled.
	- RTP_SUPPORT_IPV6MULTICAST:
		If set, IPv6 multicasting support is enabled.
	- RTPDEBUG:
		Enables some memory tracking functions and some debug routines.

SHORT-TERM RTP REPAIR CACHE
===========================

The optional RTPRepairHistory in src/rtpretransmit.h is disabled by default.
RTPSession::ConfigureRepair wires the same history into the normal send and
incoming Generic NACK paths; RTPPacketBuilder also exposes the shared history.
When it is not configured, ordinary SendPacket/SendPacketEx behaviour and the
clock are untouched.

Only complete, legal RTP packets whose underlying write succeeded are submitted
with RememberSent/SentForRepair.  The history allocates and owns an independent
copy of each complete original RTP packet; callers retain ownership of the
buffer passed to RememberSent and of the clock/writer context pointers.
CreateTransmitter results are owned by RTPSession and are deleted by Destroy.
Configure copies the POD configuration and a successful configuration clears
the previous history.  Clear, Destroy and an SSRC generation change release
stored packets; RTPSession does not own the user-supplied clock context.

PacketCount() is the number of packets currently retained after deadline
cleanup and quota eviction.  ByteCount() is the sum of the complete original
wire lengths of those retained packets, including RTP headers, CSRC lists,
header extensions and padding; it is not the builder's payload-octet counter.
Expired and quota-evicted packets are excluded from both values.  Successful
retransmissions write the stored original bytes again but do not build a new
packet and do not advance the normal media sequence number, RTP timestamp,
packet count or payload-octet count.

------------------------------------------------------------------------------

