# mqtt-go

[![Go Reference](https://pkg.go.dev/badge/github.com/at-wat/mqtt-go.svg)](https://pkg.go.dev/github.com/at-wat/mqtt-go) [![ci](https://github.com/at-wat/mqtt-go/actions/workflows/ci.yml/badge.svg)](https://github.com/at-wat/mqtt-go/actions/workflows/ci.yml) [![codecov](https://codecov.io/gh/at-wat/mqtt-go/branch/master/graph/badge.svg)](https://codecov.io/gh/at-wat/mqtt-go) [![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## Yet another Go MQTT 3.1.1 client library

- Go-ish interface
  > Fully context controlled and mockable interface.
- Extensible
  > Easy to implement a wrapper with unified interface. e.g. AWS IoT WebSocket dialer with automatic presign URL updater is available: [AWS IoT Device SDK for Go](https://github.com/seqsense/aws-iot-device-sdk-go)
- Thread-safe
  > All functions and structs are safe to be used from multiple goroutines.

## Outbound packet identifier ownership

Every `BaseClient` transport has one outbound packet identifier namespace
shared by QoS1/QoS2 `PUBLISH`, `SUBSCRIBE` and `UNSUBSCRIBE`. An identifier is
an owned, in-flight resource rather than a counter value:

- Identifiers are 1 to 65535. `0` is never allocated and means "assign
  automatically" in `Message.ID`.
- Automatic allocation scans past every identifier that is currently owned on
  the transport, including ones that wrapped around. Independent
  `BaseClient`s have independent namespaces, and QoS0 publishes do not take an
  identifier.
- If all identifiers are owned, the call returns `ErrPacketIdentifiersExhausted`
  without writing to the transport. If `Message.ID` explicitly names an
  identifier already owned by another in-flight operation, the call returns
  `ErrPacketIdentifierInUse` without changing the identifier.
- Ownership is released only at the end of the protocol exchange: `SUBACK`
  ends a subscribe, `UNSUBACK` ends an unsubscribe, `PUBACK` ends a QoS1
  publish, and `PUBCOMP` ends a QoS2 publish. A QoS2 publish keeps its
  identifier through `PUBREC`/`PUBREL` until `PUBCOMP`.
- A caller timing out does not release the identifier: the command may still
  be outstanding on the broker. A late terminal acknowledgement settles only
  that old operation. Remaining leases are reclaimed when the transport ends;
  a new connection generation has its own namespace and is unaffected by a
  late cleanup of the previous one.
- Retrying the same logical operation on the same transport reuses its own
  identifier and protocol phase. Retrying it on another transport reserves a
  fresh identifier there. An explicitly specified identifier is never silently
  replaced by a different one.

## Migration guide

- [v0.14.0](MIGRATION.md#v0140)
- [v0.12.0](MIGRATION.md#v0120)

## Examples

- [MQTTs with client certificate](examples/mqtts-client-cert)
- [WebSockets with presigned URL](examples/wss-presign-url)

## Reference

- [MQTT Version 3.1.1 Plus Errata 01](http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/mqtt-v3.1.1.html)

## License

This package is licensed under [Apache License Version 2.0](./LICENSE).
