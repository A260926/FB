# Constrained Application Protocol Client and Server for go

You can read more about CoAP in [RFC 7252][coap].  I also did
some preliminary work on `SUBSCRIBE` support from
[an early draft][shelby].

[shelby]: http://tools.ietf.org/html/draft-shelby-core-coap-01
[coap]: http://tools.ietf.org/html/rfc7252

## Sessions and connection ownership

`NewSession(pc, peer, cfg)` creates a `Session` that runs concurrent
confirmable request/response exchanges with a single peer, and **takes
ownership of the packet connection**: after the call, only the session
may read from, write to, or close `pc`.  `Session.Close` ends every
waiting request and closes the connection; it is safe to call more
than once.  The session only accepts datagrams from `peer`.

Unlike the low-level `Conn.Send` (which only waits for the next single
message), `Session.Request` treats "transmission confirmed" and
"response received" separately: an empty ACK just stops retransmission
while the request keeps waiting for a piggybacked or separate
response.  Message IDs and tokens are supplied by the caller and stay
reserved until the exchange lifetime from the first send expires.

The older `Conn` entry points (`Dial`, `Send`, `Receive`) are
unchanged and do not share state with `Session`.

See `examples/parallel` for issuing several requests concurrently over
one session, and `examples/collect` for a full device-interface
collection flow (parallel reads, record parsing, dedup, JSON Lines
output) against a simulated local device.
