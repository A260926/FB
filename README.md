
[![CI Linux](https://github.com/pjsip/pjproject/actions/workflows/ci-linux.yml/badge.svg?branch=master)](https://github.com/pjsip/pjproject/actions/workflows/ci-linux.yml)
[![CI Mac](https://github.com/pjsip/pjproject/actions/workflows/ci-mac.yml/badge.svg?branch=master)](https://github.com/pjsip/pjproject/actions/workflows/ci-mac.yml)
[![CI Windows](https://github.com/pjsip/pjproject/actions/workflows/ci-win.yml/badge.svg?branch=master)](https://github.com/pjsip/pjproject/actions/workflows/ci-win.yml)
[![Bitrise iOS](https://img.shields.io/bitrise/70e79dc5-cae8-4cb7-a6cd-9a5bd3f3270f?token=tnXk2DZ71Zmd0qDMhFgiBg&label=CI%20iOS)](https://app.bitrise.io/app/70e79dc5-cae8-4cb7-a6cd-9a5bd3f3270f)
[![Bitrise Android](https://img.shields.io/bitrise/e4b6aade20ea9eb3?token=byZU0e1BJn_VYg2YuAs-cA&label=CI%20Android)](https://app.bitrise.io/app/e4b6aade20ea9eb3)
<BR>
[![OSS-Fuzz](https://oss-fuzz-build-logs.storage.googleapis.com/badges/pjsip.png)](https://oss-fuzz-build-logs.storage.googleapis.com/index.html#pjsip)
[![Coverity-Scan](https://scan.coverity.com/projects/905/badge.svg)](https://scan.coverity.com/projects/pjsip)
[![CodeQL](https://github.com/pjsip/pjproject/actions/workflows/codeql-analysis.yml/badge.svg?branch=master)](https://github.com/pjsip/pjproject/actions/workflows/codeql-analysis.yml)
[![docs.pjsip.org](https://readthedocs.org/projects/pjsip/badge/?version=latest)](https://docs.pjsip.org/en/latest/)


# PJSIP

PJSIP is a free and open source multimedia communication library written in C with high level API in C, C++, Java, C#, and Python languages. It implements standard based protocols such as SIP, SDP, RTP, STUN, TURN, and ICE. It combines signaling protocol (SIP) with rich multimedia framework and NAT traversal functionality into high level API that is portable and suitable for almost any type of systems ranging from desktops, embedded systems, to mobile handsets.

## Getting PJSIP

- Main repository: https://github.com/pjsip/pjproject
- Releases: https://github.com/pjsip/pjproject/releases


## Documentation

Main documentation site: https://docs.pjsip.org

Table of contents:

- Overview
  - [Overview](https://docs.pjsip.org/en/latest/overview/intro.html)
  - [Features (Datasheet)](https://docs.pjsip.org/en/latest/overview/features.html)
  - [License](https://docs.pjsip.org/en/latest/overview/license.html)
- **Getting started**
  - [Getting PJSIP](https://docs.pjsip.org/en/latest/get-started/getting.html)
  - [General Guidelines](https://docs.pjsip.org/en/latest/get-started/general_guidelines.html)
  - [Android](https://docs.pjsip.org/en/latest/get-started/android/index.html)
  - [iPhone](https://docs.pjsip.org/en/latest/get-started/ios/index.html)
  - [Mac/Linux/Unix](https://docs.pjsip.org/en/latest/get-started/posix/index.html)
  - [Windows](https://docs.pjsip.org/en/latest/get-started/windows/index.html)
  - [Windows Phone](https://docs.pjsip.org/en/latest/get-started/windows-phone/index.html)
- PJSUA2 - High level API guide
  - [Introduction](https://docs.pjsip.org/en/latest/pjsua2/intro.html)
  - [Building PJSUA2](https://docs.pjsip.org/en/latest/pjsua2/building.html)
  - [General concepts](https://docs.pjsip.org/en/latest/pjsua2/general_concept.html)
  - [Hello world!](https://docs.pjsip.org/en/latest/pjsua2/building.html)
  - [Using PJSUA2](https://docs.pjsip.org/en/latest/pjsua2/using/index.html)
  - [Sample applications](https://docs.pjsip.org/en/latest/pjsua2/samples.html)
- Specific guides
  - [Audio](https://docs.pjsip.org/en/latest/specific-guides/index.html#audio)
  - [Audio Troubleshooting](https://docs.pjsip.org/en/latest/specific-guides/index.html#audio-troubleshooting)
  - [Build and integration](https://docs.pjsip.org/en/latest/specific-guides/index.html#build-integration)
  - [Development and programming](https://docs.pjsip.org/en/latest/specific-guides/index.html#development-programming)
  - [Media](https://docs.pjsip.org/en/latest/specific-guides/index.html#media)
  - [Network and NAT](https://docs.pjsip.org/en/latest/specific-guides/index.html#network-nat)
  - [Performance and footprint](https://docs.pjsip.org/en/latest/specific-guides/index.html#performance-footprint)
  - [Security](https://docs.pjsip.org/en/latest/specific-guides/index.html#security)
  - [SIP](https://docs.pjsip.org/en/latest/specific-guides/index.html#sip)
  - [Video](https://docs.pjsip.org/en/latest/specific-guides/index.html#video)
  - [Other](https://docs.pjsip.org/en/latest/specific-guides/index.html#other)
- API reference
  - [PJSUA2](https://docs.pjsip.org/en/latest/api/pjsua2/index.html) - high level API (Java/C#/Python/C++/swig)
  - [PJSUA-LIB](https://docs.pjsip.org/en/latest/api/pjsua-lib/index.html) - high level API (C)
  - [PJSIP](https://docs.pjsip.org/en/latest/api/pjsip/index.html) - SIP stack
  - [PJMEDIA](https://docs.pjsip.org/en/latest/api/pjmedia/index.html) - media framework
  - [PJNATH](https://docs.pjsip.org/en/latest/api/pjnath/index.html) - NAT traversal helper
  - [PJLIB-UTIL](https://docs.pjsip.org/en/latest/api/pjlib-util/index.html) - utilities
  - [PJLIB](https://docs.pjsip.org/en/latest/api/pjlib/index.html) - portable library

## Manual-clock RTP source splice

`pjmedia_rtp_splice` (declared in `pjmedia/include/pjmedia/rtp.h`,
implemented in `pjmedia/src/pjmedia/rtp_splice.c`) continues playback
between two RTP sources that carry the same encoding and packet duration.
It never opens a socket or a thread: the caller feeds complete packets and
advances a microsecond clock.

```c
#include <pjmedia/rtp.h>
#include <stdlib.h>

static pjmedia_rtp_splice_cfg cfg = {
    0x11111111,   /* init_ssrc: source that plays first       */
    0x99999999,   /* out_ssrc: SSRC written to every output    */
    96,           /* pt                                        */
    8000,         /* clock_rate                                */
    160,          /* frame_ticks: 20 ms at 8 kHz               */
    100000,       /* delay_us: 100 ms playout delay            */
    16,           /* window: slots                                 */
    0xFFFE,       /* out_seq: sequence of output slot zero    */
    0xFFFFF000    /* out_ts: timestamp of output slot zero    */
};

pjmedia_rtp_splice *splice;
void *out;
pj_size_t out_len;
pj_uint64_t now = 0;               /* caller-owned microsecond clock */

pjmedia_rtp_splice_alloc(&splice, &cfg);

/* Packets are supplied exactly as received; bytes are copied. */
pjmedia_rtp_splice_push(splice, pkt_a0, len_a0, now);

/* Queue an output packet once its slot is due.  The returned buffer is
 * malloc()ed, keeps the whole packet, and only SSRC/seq/ts are rewritten. */
while (pjmedia_rtp_splice_pop(splice, now, &out, &out_len) == 0) {
    send_packet(out, out_len);
    free(out);
}

/* Hand over to 0x22222222 at logical slot 'head + 2'.  The candidate is
 * proven by its packets for slots c, c+1 and c+2; the switch happens when
 * pop() first advances to c.  Before that the old source still plays. */
pjmedia_rtp_splice_arm(splice, 0x22222222,
                       pjmedia_rtp_splice_head(splice) + 2);
pjmedia_rtp_splice_push(splice, pkt_b0, len_b0, now);   /* maps to slot c */
pjmedia_rtp_splice_push(splice, pkt_b1, len_b1, now);   /* c + 1           */
pjmedia_rtp_splice_push(splice, pkt_b2, len_b2, now);   /* c + 2, ready    */

/* If the proof is incomplete when the head reaches c, the candidate is
 * dropped and the old source keeps playing.  A successful switch frees the
 * old source from c onwards and rejects it until it is armed again.
 * pjmedia_rtp_splice_cancel() drops only the candidate;
 * pjmedia_rtp_splice_reset() releases everything and re-anchors the
 * configured initial source. */

pjmedia_rtp_splice_destroy(splice);
```

Run `bash task_support/run.sh baseline` for the ordinary RTP send/receive
header path and `bash task_support/run.sh target` for the production
implementation plus the `tests/splice.c` regression suite.
