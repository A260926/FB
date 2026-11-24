# Splint annotations added in this tree

## `transferswhen`

A status-conditioned ownership hand-off is written as a function annotation:

```c
int acquire(/*@special@*/ object *src,
            /*@special@*/ object **dst)
  /*@transferswhen 0 src -> *dst@*/;
```

It means:

* when the function returns the specified integer status (`0` in the
  example, negative constants such as `-4` are also allowed), the object
  owned through `src` on entry is the same object reachable through `*dst`;
* the caller no longer owns `src` on that status and must manage `*dst`;
* on every other return status there is no transfer, so `src` remains the
  caller's obligation and `*dst` keeps its pre-call state;
* `src` and `dst` must both be declared `special`.  At the call site `src`
  must denote an `only` object, while `dst` is a pointer-to-pointer output
  parameter.  This avoids treating an ordinary `only` source parameter as
  unconditionally consumed.

Several relations may appear on one function.  For one status, repeating the
same source consumption or repeating the same target write is rejected when
the declaration is checked.  Different statuses may reuse a source or target.

The implementation checks every reachable return of a defined function against
the entry identity.  A successful return that writes a different/new object,
or a non-success return that has already moved the entry object, is reported as
`conditional ownership` with the relation, function, return location and the
declaration location.

Callers recognize direct comparisons, negated zero tests, an integer result
stored in a local before comparison, and early returns from one branch.
Outputs may be `&local` or a one-level pointer alias initialized from `&local`.
Reassigning a resource or result ends the previous relation; repeated use of a
transferred source, unresolved control-flow joins, and release obligations
left live are diagnosed with both call and declaration locations.

Ordinary `only`, `ensures`, and existing library annotations continue to use
their normal rules; transfer protocols are never inferred from a function
name.

## Libraries

`-dump name` records `transferswhen` relations in the Splint library data and
also writes a `name.rs` sidecar.  `-load name` restores them when analyzing
another source file.  Libraries without a result-state section are treated as
old-format Splint libraries.  A result-state section whose format version is
newer than the supported version is rejected with an explicit
`unsupported resultState library format version ...` diagnostic.
