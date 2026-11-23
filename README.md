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

### Receive-side repair before the jitter buffer

On the inbound path, register the FlexFEC repair receiver **first**, then the
jitter buffer (or any other reordering stage). The repair interceptor consumes
the interleaved media and repair datagrams of one bound stream, solves the
FlexFEC-03 equations incrementally over GF(2) — including multi-row systems
where no single equation peels immediately — and only ever hands media RTP to
the next stage, so the jitter buffer sees original and recovered packets on
one continuous sequence space.

```go
repair, err := flexfec.NewRepairReceiver(32 /*window*/, 32 /*maxEquations*/)
if err != nil { panic(err) }

info := &interceptor.StreamInfo{
    SSRC:                              mediaSSRC,
    PayloadType:                       mediaPT,
    SSRCForwardErrorCorrection:        fecSSRC,
    PayloadTypeForwardErrorCorrection: fecPT,
}

// repair first, jitter buffer second
mediaReader := repair.BindRemoteStream(info, upstreamReader)
mediaReader = jitterBuffer.BindRemoteStream(info, mediaReader)
```

`Read` delivers queued media first and otherwise consumes at most one upstream
datagram. When a read produces no media (for example a repair-only datagram, or
equations that are not yet full rank) it returns `flexfec.ErrRepairPending`;
retry the read. Upstream errors are returned unchanged, without dropping media
that was already recovered.

Attributes:

* Original media packets keep the attributes of the datagram that carried
  them (each input's attributes are snapshotted, so caller-reused maps cannot
  mutate stored state).
* Recovered packets carry a snapshot of the attributes of the last input
  packet that contributed to solving their equation, plus the boolean
  attribute `flexfec.recovered=true`. Cached RTP headers from input bytes are
  never reused on recovered bytes, since those describe a different packet.
* A complete example is in
  [examples/flexfec-receive](https://github.com/pion/interceptor/tree/master/examples/flexfec-receive).


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
