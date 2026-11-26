# c-ares replayable property / metamorphic tests

These tests check whether the real c-ares parser still agrees with an
*independent* logical model after legal changes of representation
(compression layout, record ordering, name casing, unrelated records).
The whole chain goes through the shipped library interfaces
`ares_parse_a_reply()`, `ares_parse_aaaa_reply()` and
`ares_expand_name()`; there is no demo parser and no network or resolver.

Everything is C99 plus POSIX shell. No new packages are required and no
network access is performed. The generator and oracle live in a single
file, `task_support/target.c`, so `task_support/check.sh` needs no extra
source arguments. `target.c` exposes `int task_cases(const char* group)`
and uses `TASK_CHECK` from `task_support/check.h`, exactly as the driver
expects.

## Build and run

```sh
cmake -S . -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCARES_STATIC=ON -DCARES_SHARED=OFF \
  -DCARES_BUILD_TESTS=OFF -DCARES_BUILD_TOOLS=OFF
cmake --build .task-build -j 4
sh task_support/baseline.sh   # original library smoke call only
sh task_support/check.sh      # baseline + the property tests below
```

`baseline.sh` only exercises an unmodified library call
(`ares_expand_name` on a compressed name). `check.sh` reruns it and then
compiles and runs the property target.

## Logical model and syntax scope

A name is an array of ASCII labels. Labels contain only letters, digits
and hyphens (`[A-Za-z0-9-]`), each at most 63 wire bytes. The root is the
zero-label name and renders as the empty string, matching
`ares_expand_name`. Every non-root name in a case shares a generated zone
so legal suffix compression is always available.

Each of the fixed 500 cases holds:

* `k` CNAME hops, `k` uniformly in 1..4: `N0 -> N1 -> ... -> Nk`;
* 1..3 final records of a single family, A (4 bytes) or AAAA (16 bytes);
* a deterministic set of addresses.

Every name/name-occurrence obeys the DNS limits: message <= 512 bytes,
on-wire name <= 255 bytes, label <= 63 bytes. The encoder tracks the
exact wire byte length of every name occurrence and compares it with the
`enclen` returned by `ares_expand_name`.

### Three legal layouts

* `none`  - no compression, every name written literally;
* `suffix`- suffix-only compression; every pointer keeps at least one
  literal label and targets a depth-0 label boundary written earlier;
* `multi` - cross-record multi-level suffix compression; whole-name
  pointers and pointer chains are allowed, still only to earlier label
  boundaries. A backward pointer ladder (depth 1..4) is emitted in the
  additional section so multi-hop decoding is genuinely exercised.

Pointers are two bytes with the `11` prefix and may only reference a label
boundary already emitted; an internal counter (`ptr_illegal`) would fail
the run if a forward/self pointer were ever produced. Pointer depth is
capped at 8, well under c-ares' own 50-indirection guard.

## Oracle independence

The model, encoder and expected values are computed without calling
c-ares, and expectations are never derived from a library result. The
oracle compares the parsed `hostent` against the model:

* addresses by exact bytes, with wire order preserved;
* the canonical name and aliases by DNS ASCII identity - exact wire case
  by default, case-insensitive folding for the query-name case transform;
* every name occurrence's expanded text and `enclen`.

The oracle encodes the *observed* c-ares semantics (see limitations): with
CNAMEs present, `h_name` is the first CNAME target and `h_aliases` lists
the CNAME owners in wire order.

## Metamorphic transforms

For every case and every layout, three legal transformations must leave
the question/answer identity and the final address set unchanged:

* record-order adjustment - two unrelated RRs inserted at three slots;
* query-name case flip - compared case-insensitively;
* unrelated additional-section records (TXT plus an A/AAAA RR).

Unrelated owners never match the tracked hostname, so the parser must
ignore them. 7,500 transform runs are checked per pass.

## Seed replay

Generation uses splitmix64 from the compile-time constant
`FIXED_SEED`. Re-seeding regenerates byte-identical messages:

* `replay_byte_identical()` rebuilds every model/message and compares
  bytes;
* an FNV-1a checksum over all 1,500 baseline messages is recomputed and
  must equal the checksum accumulated during the main pass.

Each case line prints its own seed (`CASE nnn seed=... chain=...`) so any
single case can be reproduced by re-seeding.

## Reading the coverage output

* `COVERAGE` - case/message counts, the histogram of CNAME chain lengths
  (1..4), A vs AAAA counts, root cases, root one-byte vs two-byte
  (pointer) encodings, and the pointer-depth histogram across all name
  occurrences.
* `ENCLEN` - number of `ares_expand_name` length checks (zero failures
  expected), exact/folded name comparisons, metamorphic run and
  invariant counts, and the size/pointer-limit counters.
* `FIRST_DIVERGENCE` - the first case and layout that disagree, or
  `none`. On a healthy build it is always `none`.
* `DIFF_SIMULATOR` - result of the fault-injection check.

Representative numbers from the committed fixed seed (500 cases):

```
COVERAGE cases=500 messages=1500 parses_ok=1500
  chain_k={1:131,2:136,3:123,4:110} A=257 AAAA=243 root_cases=32
  root_1byte=1152 root_2byte=288
  depth={0:5010,1:5550,2:2134,3:500,4:500,>=5:0}
ENCLEN checks=84164 failures=0 names_exact=29340 names_fold=30816
  meta_runs=7500 meta_invariant=7500 meta_fail=0
  size_violations=0 ptr_illegal=0
```

Coverage is enforced by assertions, not by skipping work: each chain
length must appear at least 50 times, both families at least 100 times,
root cases at least 20, and pointer depths 3 and 4 at least 100 times.

## Logic-difference simulator

`difference_simulator()` corrupts only the oracle's *expectation* - it
flips one expected address byte on pre-determined cases 3 and 9 and
appends a byte to an expected canonical name. Library code and the
wire messages are untouched. The oracle must return the matching error
class, keep the first failing case (3), and never raise a false positive
on the other cases. This proves the oracle can actually fail; the
upstream c-ares sources are not modified to manufacture a defect.

## Result limitations

* The oracle targets the c-ares behaviour found in this tree: with
  CNAMEs, `h_name` is the first CNAME target and `h_aliases` holds the
  CNAME owners in wire order. A future parser change here is exactly the
  kind of divergence these tests are meant to surface.
* Only class IN, types A/AAAA/CNAME (plus inert TXT/A/AAAA additional
  records) and the three stated layouts are generated; all messages are
  protocol-legal, so malformed-input tolerance is only spot-checked via
  the truncation test.
* The additional-section pointer ladder is validated through
  `ares_expand_name` (text and `enclen`); the A/AAAA parser intentionally
  never consumes the additional section, so those owners do not appear in
  the `hostent`.
* No network resolver or live DNS service is used, and the program is
  fully deterministic; timing, caching and transport behaviour are out of
  scope.
