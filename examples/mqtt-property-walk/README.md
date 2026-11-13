# MQTT5 property walk (in-memory)

This example walks the MQTT5 properties of a PUBLISH packet that is already
in memory. It does **not** open a socket, start a broker, or contact a server:
the packet is a byte array in `main.c`. It only uses the parser and the local
buffer.

```sh
make              # builds ./example and runs it
cc -std=c99 -I. main.c mongoose.c -o example
./example
```

## The decoder

```c
int mg_mqtt_prop_next(const struct mg_mqtt_message *msg,
                      struct mg_mqtt_prop *prop, size_t *offset);
```

`*offset` is a byte offset from the first byte of the property section. Pass
`0` to start and feed the returned offset back to continue:

| return | meaning |
|--------|---------|
| `1` | one property decoded; `*prop` and `*offset` are updated |
| `0` | `*offset` is exactly at the end of the property section |
| `-1` | invalid section description, `*offset` outside the section, unknown property id, or a property that does not end inside the section |

On `0` and `-1`, `*prop` and `*offset` are left exactly as they were at entry,
so a caller never receives half a string or half a user property. The decoder
never reads outside the described property section: a property that would need
bytes from the payload or from a following packet is `-1`, not a silent read.

The section itself is described by `msg->props_start` (offset into
`msg->dgram.buf`) and `msg->props_size`. If those values do not fit inside
`msg->dgram.len`, every call returns `-1`. `msg->dgram.buf` must point to
`msg->dgram.len` readable bytes; it may be `NULL` only when the length is zero.
All numeric fields are converted to the field's actual type, so nothing
overwrites neighbouring fields on 32-bit or 64-bit `size_t` targets.

Supported representations are the seven in the property table: byte, short,
int, variable integer, string, binary data, and string pair. Values go to `iv`,
strings and binary data to `val`, user properties to `key` and `val`. Unused
fields are zeroed on success. Both length prefixes are network-order 16-bit
values. Variable integers are at most four bytes and must terminate; the
library's existing numeric decoding is kept, so non-shortest encodings are
still accepted. Zero-length strings, binary values, and empty halves of a user
property are valid, and zero bytes inside binary data are preserved.

## Borrowed views

`prop->key.buf` and `prop->val.buf` point directly into `msg->dgram.buf`. They
remain valid only while that buffer is alive and unmodified. Copy the bytes out
if the packet buffer may be reused, freed, or overwritten (for example when the
receive buffer is compacted after an event callback).

## Packet framing

`mg_mqtt_parse()` validates the same boundaries for MQTT5 PUBLISH:

- Not all of the bytes announced by the fixed header are present:
  `MQTT_INCOMPLETE`.
- The packet is complete, but the topic, packet id, property length, or a
  property entry is inconsistent: `MQTT_MALFORMED`.
- Otherwise `MQTT_OK`, only after the whole property section has been
  validated. `props_start`, `props_size`, and `data` then describe exactly the
  parsed packet region.

A zero-length property section, an empty payload, and a one-byte payload are
all handled explicitly; the parser never guesses the property length from
"how many bytes are left". QoS 0/1/2 layouts are unchanged, only the first
packet in a concatenated buffer is parsed, and `dgram.len` is that first
packet's true length. Valid MQTT 3.1.1 (version 4) packets keep their old
behaviour.

## Legacy helper

`mg_mqtt_next_prop()` keeps its signature. On success it returns the next
relative offset; at the end of the section and on errors it returns `0`. It
uses the same checked decoder, so there is no unchecked parsing path left.

## Tests

The memory-safety regression lives in `test/mqtt_property_contract.c` with the
shared model in `test/mqtt_property_core.h`; the libFuzzer entry point is
`test/mqtt_property_fuzz.c`. See those files for the values, boundaries,
framing, and fuzz groups.
