# evbuffer atomic-failure compatibility notes

This note describes the failure-atomicity hardening of two in-process
`evbuffer` entry points and the compatibility impact on existing callers.

## Scope

Only two functions change behavior:

- `evbuffer_add_buffer_reference(outbuf, inbuf)`
- `evbuffer_remove_buffer(src, dst, datlen)`

No public signature, return type, header, struct layout or ABI changes.
Data sizes remain bounded by `INT_MAX` and the existing `int` return type of
`evbuffer_remove_buffer()` is preserved. No networking, threading model,
OpenSSL or new download dependency is involved.

## Failure contract

On allocation failure, or on any pre-existing rejection condition, both
functions return `-1` and leave the source and destination buffers identical
to their state immediately before the call:

- visible bytes and `evbuffer_get_length()` are unchanged;
- the chain structure, original block reference relationships and the
  `IMMUTABLE` state are unchanged;
- buffer and chain reference counts, and therefore cleanup responsibility,
  are unchanged (an external block's cleanup callback is neither triggered
  early nor invoked twice);
- no change event for this call is produced: `n_added`/`n_deleted` are not
  incremented and callbacks already queued (including deferred ones) are left
  intact.

A partial result is never reported as success, and a failed copy never causes
bytes to be removed from the source.

## Success contract (unchanged)

- `evbuffer_add_buffer_reference()` still performs a non-destructive
  snapshot. The multicast chains keep pointing at the original data storage;
  the source is not copied into a temporary contiguous buffer. Only the
  cross-chain boundary fragment in a transfer may be copied, exactly as
  before.
- `evbuffer_remove_buffer()` still transfers whole source chains by pointer
  surgery (no per-byte copy of complete chains) and returns
  `min(requested, source length)` bytes.
- Self operations, zero-length requests, frozen front/back ends, and the
  unsupported multicast/file/sendfile source chains keep their existing
  documented behavior and return codes.

## Implementation boundary

Both functions now prepare every chain descriptor and any required tail
storage before mutating the buffers, then commit in a single step that cannot
fail. On failure the prepared, detached resources are released. The evbuffer
chain representation is retained throughout; whole-chain transfer is not
converted into copying.

The existing locking discipline is unchanged: the same recursive buffer
locks are taken in the same order, so single-threaded behavior is identical
and multi-threaded callers keep their prior guarantees.

## Pre-existing limitations left untouched

Two unrelated, pre-existing chain-lifetime hazards in other code paths were
observed while building the randomized model and are intentionally **not**
part of this change (they reproduce byte-for-byte on the unmodified library):

1. Moving a multicast chain back into a buffer that is (transitively) its own
   reference source forms a holder/source reference-count cycle.
2. `evbuffer_pullup()` on a buffer holding shared multicast chains can reuse
   shared immutable storage.

The model test deliberately avoids constructing those unsupported
combinations rather than masking them. They do not affect the atomicity of
the two entry points above.
