<h1 align="center">
  <br>
  Pion Interceptor
  <br>
</h1>
<h4 align="center">RTP and RTCP processors for building real time communications</h4>
<p align="center">
  <a href="https://pion.ly"><img src="https://img.shields.io/badge/pion-interceptor-gray.svg?longCache=true&colorB=brightgreen" alt="Pion Interceptor"></a>
  <a href="https://discord.gg/PngbdqpFbt"><img src="https://img.shields.io/badge/join-us%20on%20discord-gray.svg?longCache=true&logo=discord&colorB=brightblue" alt="join us on Discord"></a> <a href="https://bsky.app/profile/pion.ly"><img src="https://img.shields.io/badge/follow-us%20on%20bluesky-gray.svg?longCache=true&logo=bluesky&colorB=brightblue" alt="Follow us on Bluesky"></a>
  <br>
  <img alt="GitHub Workflow Status" src="https://img.shields.io/github/actions/workflow/status/pion/interceptor/test.yaml">
  <a href="https://pkg.go.dev/github.com/pion/interceptor"><img src="https://pkg.go.dev/badge/github.com/pion/interceptor.svg" alt="Go Reference"></a>
  <a href="https://codecov.io/gh/pion/interceptor"><img src="https://codecov.io/gh/pion/interceptor/branch/master/graph/badge.svg" alt="Coverage Status"></a>
  <a href="https://goreportcard.com/report/github.com/pion/interceptor"><img src="https://goreportcard.com/badge/github.com/pion/interceptor" alt="Go Report Card"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>
<br>

Interceptor is a framework for building RTP/RTCP communication software. This framework defines
a interface that each interceptor must satisfy. These interceptors are then run sequentially. We
also then provide common interceptors that will be useful for building RTC software.

This package was built for [pion/webrtc](https://github.com/pion/webrtc), but we designed it to be consumable
by anyone. With the following tenets in mind.

* Useful defaults. Each interceptor will be configured to give you a good default experience.
* Unblock unique use cases. New use cases are what is driving WebRTC, we want to empower them.
* Encourage modification. Add your own interceptors without forking. Mixing with the ones we provide.
* Empower learning. This code base should be useful to read and learn even if you aren't using Pion.

### Current Interceptors
* [NACK Generator/Responder](https://github.com/pion/interceptor/tree/master/pkg/nack)
* [Sender and Receiver Reports](https://github.com/pion/interceptor/tree/master/pkg/report)
* [Transport Wide Congestion Control Feedback](https://github.com/pion/interceptor/tree/master/pkg/twcc)
* [RTCP Feedback for Congestion Control](https://github.com/pion/interceptor/tree/master/pkg/rfc8888) as defined by [RFC 8888](https://datatracker.ietf.org/doc/html/rfc8888).
* [JitterBuffer](https://github.com/pion/interceptor/tree/master/pkg/jitterbuffer) Re-order packets and wait for arrival on the remote/inbound RTP path.
* [Packet Dump](https://github.com/pion/interceptor/tree/master/pkg/packetdump)
* [Google Congestion Control](https://github.com/pion/interceptor/tree/master/pkg/gcc)
* [Stats](https://github.com/pion/interceptor/tree/master/pkg/stats) A [webrtc-stats](https://www.w3.org/TR/webrtc-stats/) compliant statistics generation
* [Interval PLI](https://github.com/pion/interceptor/tree/master/pkg/intervalpli) Generate PLI on a interval. Useful when no decoder is available.
* [FlexFec](https://github.com/pion/interceptor/tree/master/pkg/flexfec) – [FlexFEC-03](https://datatracker.ietf.org/doc/html/draft-ietf-payload-flexible-fec-scheme-03) encoder implementation

### Planned Interceptors
* Bandwidth Estimation
  - [NADA](https://tools.ietf.org/html/rfc8698)

### Interceptor Public API
The public interface is defined in [interceptor.go](https://github.com/pion/interceptor/blob/master/interceptor.go).
The methods you need to satisy are broken up into 4 groups.

* `BindRTCPWriter` and `BindRTCPReader` allow you to inspect/modify RTCP traffic.
* `BindLocalStream` and `BindRemoteStream` notify you of a new SSRC stream and allow you to inspect/modify.
* `UnbindLocalStream` and `UnbindRemoteStream` notify you when a SSRC stream has been removed
* `Close` called when the interceptor is closed.

Interceptors also pass Attributes between each other. These are a collection of key/value pairs and are useful for storing metadata
or caching.

[noop.go](https://github.com/pion/interceptor/blob/master/noop.go) is an interceptor that satisfies this interface, but does nothing.
You can embed this interceptor as a starting point so you only need to define exactly what you need.

[chain.go]( https://github.com/pion/interceptor/blob/master/chain.go) is used to combine multiple interceptors into one. They are called
sequentially as the packet moves through them.

### Examples
The [examples](https://github.com/pion/interceptor/blob/master/examples) directory provides some basic examples. If you need more please file an issue!
You should also look in [pion/webrtc](https://github.com/pion/webrtc) for real world examples.

### Redundant dual-path jitter mode

`pkg/jitterbuffer` also provides an explicit redundant reception mode for the same RTP media sent over two SSRCs with a fixed sequence-number offset:

```go
jb, err := jitterbuffer.NewRedundant(jitterbuffer.RedundantConfig{
    OutputSSRC: 424242,
    PayloadType: 96,
    Paths: [2]jitterbuffer.RedundantPath{
        {SSRC: 1001, SequenceOffset: 7, RTT: 10 * time.Millisecond},
        {SSRC: 2002, SequenceOffset: 99, RTT: 30 * time.Millisecond},
    },
    Window: 16,
    Hold:   20 * time.Millisecond,
    Retry:  50 * time.Millisecond,
    Now:    time.Now, // tests/callers can supply a manually advanced clock
})
```

Path sequence numbers are converted to the canonical domain with
`canonical = pathSequence - RedundantPath.SequenceOffset` (modulo 65536). The first accepted copy owns the packet content and attributes; duplicates consume no additional canonical capacity. `Push` and `Pop` remain the data path. A revealed head gap waits for `Hold`; after that time `Pop` retires the gap and jumps to the earliest received packet. Empty/not-ready reads return `ErrRedundantPending`. The other positional/peek APIs return `ErrRedundantMode`, and `SetPlayoutHead` is a no-op in this mode. `Clear(false)` removes packets and feedback state while retaining the playout head; `Clear(true)` also resets the head.

Use `jitterbuffer.WithRedundant(cfg)` as a `NewInterceptor` option to install the RTP readers and RTCP feedback translator. Readers for the two configured SSRCs share one canonical window while still reading only their own upstream reader. Readers for other SSRCs are returned unchanged.

In the feedback direction, outgoing `TransportLayerNack` packets whose `MediaSSRC` is `OutputSSRC` are expanded from PID/BLP and filtered to canonical gaps that are revealed, unfilled and not retired. The first report chooses the valid path with the smaller `RTT` (Paths[0] on a tie); after a successful send, the same gap is suppressed for `Retry` and the next eligible report uses the other path. Canonical sequence numbers are converted back with `pathSequence = canonical + SequenceOffset`, then reports are regrouped by path SSRC and packed into PID/BLP pairs. `SenderSSRC` is preserved. Other RTCP packets pass through in their original order. Path binding generations ensure an unbind/rebind cannot let a late upstream write completion mutate the new generation.

A runnable merge example is available in [`examples/redundant`](examples/redundant/main.go).

### Roadmap
The library is used as a part of our WebRTC implementation. Please refer to that [roadmap](https://github.com/pion/webrtc/issues/9) to track our major milestones.

### Community
Pion has an active community on the [Discord](https://discord.gg/PngbdqpFbt).

Follow the [Pion Bluesky](https://bsky.app/profile/pion.ly) or [Pion Twitter](https://twitter.com/_pion) for project updates and important WebRTC news.

We are always looking to support **your projects**. Please reach out if you have something to build!
If you need commercial support or don't want to use public methods you can contact us at [team@pion.ly](mailto:team@pion.ly)

### Contributing
Check out the [contributing wiki](https://github.com/pion/webrtc/wiki/Contributing) to join the group of amazing people making this project possible

### License
MIT License - see [LICENSE](LICENSE) for full text
