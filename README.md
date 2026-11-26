
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

## `pjmedia_rtp_splice`：手动时钟切源 / 定时音频源续接器

`pjmedia_rtp_splice` 在 RTP 接收侧的内存里续接两路编码与包时长相同的源。
输入是调用方持有的完整 RTP 包和调用方自己推进的微秒时钟：不开 socket、
不开线程。声明在 `pjmedia/include/pjmedia/rtp.h`，生产实现是
`pjmedia/src/pjmedia/rtp_splice.c`（RTP 头/扩展解析复用生产函数
`pjmedia_rtp_decode_rtp2()`）。

要点：

- 第一个合法的初始 SSRC 包以 `(S, T, A)` 建立逻辑槽 0；槽 `k` 对应
  输入序号 `S+k`、时间戳 `T+k*frame_ticks`，期限为
  `A + delay_us + k*包间隔`。
- `push()` 复制并持有字节，同源同槽首个内容获胜；只接收
  `[head, head+window)` 内的槽。
- `pop()` 在期限前返回 `EAGAIN`；到期后每次处理一槽，有包返回 `0`、
  丢槽返回 `ENODATA`，两种情况都推进 `head`。成功时输出一块独立
  `malloc` 的包（调用方 `free()`），除 SSRC/sequence/timestamp 改写为
  `out_ssrc`、`out_seq+k`、`out_ts+k*frame_ticks` 外，原包字节全部保留。
- `arm(sp, ssrc, cut)` 武装候选源；`cut/cut+1/cut+2` 三槽收齐才就绪。
  `pop` 第一次走到 `cut` 时原子决定：就绪就切换并释放旧源 `cut` 及以后
  的缓存，未就绪就永久放弃候选、继续播旧源。
- 新接口返回 POSIX errno（`0` 成功，配置/arm 非法为 `EINVAL`，另有
  `ENOENT/ENOBUFS/ETIMEDOUT/EBADMSG/EAGAIN/ENODATA/ENOMEM`）。

### 手动时钟切源示例

下面的小程序用同一个手动变量 `now` 推进时钟，在逻辑槽 3 从 SSRC
`0xA0A0A0A0` 切到 `0xB0B0B0B0`。可直接保存为 `splice_example.c`，用
`bash task_support/build.sh target` 生成目标文件后，把它和
`.task-build/*.o` 一起编译运行：

```c
#include <pjmedia/rtp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static size_t make_rtp(uint8_t *b, uint16_t seq, uint32_t ts,
                       uint32_t ssrc, const char *payload)
{
    size_t plen = strlen(payload);
    b[0] = 0x80;                 /* RTP v2, no marker/ext/padding */
    b[1] = 96;                   /* PT */
    put16(b + 2, seq);
    put32(b + 4, ts);
    put32(b + 8, ssrc);
    memcpy(b + 12, payload, plen);
    return 12 + plen;
}

int main(void)
{
    const uint32_t SRC_A = 0xA0A0A0A0u, SRC_B = 0xB0B0B0B0u,
                   OUT_SSRC = 0xD0D0D0D0u;
    pjmedia_rtp_splice_cfg cfg;
    pjmedia_rtp_splice *sp = NULL;
    uint8_t pkt[128];
    unsigned k;

    memset(&cfg, 0, sizeof(cfg));
    cfg.ssrc        = SRC_A;     /* initial media source           */
    cfg.out_ssrc    = OUT_SSRC;  /* output identity                 */
    cfg.pt          = 96;
    cfg.clock_rate  = 8000;      /* 160 ticks -> 20 ms packet time */
    cfg.frame_ticks = 160;
    cfg.delay_us    = 40000;      /* 40 ms play-out delay           */
    cfg.window      = 16;         /* 4..512                         */
    cfg.out_seq     = 1000;
    cfg.out_ts      = 0;

    if (pjmedia_rtp_splice_alloc(&sp, &cfg) != 0)
        return 1;

    /* Source A slots 0..6. Its first packet (S=5000,T=90000) anchors
     * logical slot 0 with arrival A=now=0. */
    for (k = 0; k <= 6; ++k) {
        char pl[16];
        size_t n;
        snprintf(pl, sizeof(pl), "A%u", k);
        n = make_rtp(pkt, (uint16_t)(5000u + k), 90000u + k * 160u,
                     SRC_A, pl);
        if (pjmedia_rtp_splice_push(sp, pkt, n, 0) != 0)
            return 2;
    }

    /* Handover to SRC_B at logical cut 3. */
    if (pjmedia_rtp_splice_arm(sp, SRC_B, 3) != 0)
        return 3;

    /* Candidate coordinate at cut 3 is seq 700 / ts 40000. Slots 3,4,5
     * are the readiness proofs; slot 6 keeps the example continuous.
     * These pushes could arrive out of order. */
    for (k = 3; k <= 6; ++k) {
        char pl[16];
        size_t n;
        snprintf(pl, sizeof(pl), "B%u", k);
        n = make_rtp(pkt, (uint16_t)(700u + (k - 3)),
                     40000u + (k - 3) * 160u, SRC_B, pl);
        if (pjmedia_rtp_splice_push(sp, pkt, n, 0) != 0)
            return 4;
    }

    /* Manually advance the caller clock. Slot k is due at
     * 40000 + k*20000 us. */
    for (k = 0; k <= 6; ++k) {
        uint64_t now = 40000ull + k * 20000ull;
        void *out = NULL;
        size_t out_len = 0;
        int rc = pjmedia_rtp_splice_pop(sp, now, &out, &out_len);

        if (rc == 0) {
            uint8_t *b = (uint8_t *)out;
            uint16_t seq = (uint16_t)(((unsigned)b[2] << 8) | b[3]);
            uint32_t ts = ((uint32_t)b[4] << 24) |
                          ((uint32_t)b[5] << 16) |
                          ((uint32_t)b[6] << 8) | b[7];
            printf("now=%llu seq=%u ts=%u payload=%.*s\n",
                   (unsigned long long)now, seq, ts,
                   (int)(out_len - 12), (const char *)(b + 12));
            free(out);   /* independent malloc block from pop() */
        } else if (rc == ENODATA) {
            /* Lost slot still consumes an output sequence number. */
            printf("now=%llu slot lost (ENODATA)\n",
                   (unsigned long long)now);
        }
        /* EAGAIN: deadline for head has not been reached yet. */
    }

    pjmedia_rtp_splice_destroy(sp);
    return 0;
}
```

输出（注意逻辑槽 3 起 payload 从 `A` 换成 `B`，而输出
sequence/timestamp 连续，SSRC 始终是配置的 `out_ssrc`）：

```text
now=40000 seq=1000 ts=0 payload=A0
now=60000 seq=1001 ts=160 payload=A1
now=80000 seq=1002 ts=320 payload=A2
now=100000 seq=1003 ts=480 payload=B3
now=120000 seq=1004 ts=640 payload=B4
now=140000 seq=1005 ts=800 payload=B5
now=160000 seq=1006 ts=960 payload=B6
```

### 构建与回归

```sh
bash task_support/run.sh baseline   # 普通生产 RTP 编码/解码头往返
bash task_support/run.sh target     # 构建 rtp_splice 并运行 tests/splice.c
```

`tests/splice.c` 通过 `task_support/check.h` 注册 `timing`、`handover`、
`ownership` 三组断言；新生产文件 `pjmedia/src/pjmedia/rtp_splice.c` 也已
加入上游构建清单 `pjmedia/build/Makefile`。

