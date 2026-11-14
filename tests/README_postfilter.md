# CELT pitch post-filter content regression

`bash task_support/check_postfilter.sh` builds and runs the post-filter
regressions, then verifies that two still-compiling degeneracies are rejected.
It needs only the sources, a C compiler, CMake and the system math library.

Target build: floating point, intrinsics disabled. DRED, the neural models and
audio devices are not involved. `OPUS_POSTFILTER_TRACE` is a test-only CMake
option (default `OFF`); when enabled it adds read-only observation points in
`celt_decoder.c`. A normal build references no test callback, allocates
nothing extra and exports no new symbol (the symbol set of the plain build is
unchanged).

## The independent reference

`tests/postfilter_ref.h` evaluates the five-tap delay difference directly:

    y(n) = x(n) + g0*T(n-T) + g1*(y(n-T+1)+y(n-T-1))
                + g2*(y(n-T+2)+y(n-T-2))

The tap constants and the MDCT window come from the library's format data, but
the reference never calls `comb_filter()`, `comb_filter_const_c()`, their
constant fast paths, and it does not copy any unrolled library loop.

`tests/test_postfilter_unit.c` drives `comb_filter()` directly over the shortest
legal period (15), a period one below the block length, a period equal to the
block length, and near the maximum (1022), with all 3x3 tapset handoffs and
positive, negative and zero gains, over 120/240/960-sample blocks, in both
entry modes.

## Two properties that must not be conflated

### In-place recursion

The decoder runs the post-filter in place (`y == x`). A delayed tap of output
sample `n` can point at a sample this same call has already rewritten whenever
`n - T + d >= 0`; that written value is what the recurrence must see. The
out-of-place entry (encoder-side pre-filter, PLC fold) instead reads the
untouched input for every non-negative index.

The reference therefore evaluates one output sample at a time and reads the
output array when in place and the pristine input otherwise. The unit test runs
each parameter set through both entries and checks each against its own
reference evaluation. It also records how often the two entries differ; it
never treats their equality as a correctness condition.

### Parameter timeline

CELT keeps two generations of post-filter parameters (period, gain, tapset).
The test owns this timeline and reconstructs it, frame by frame:

* First segment: the **old** generation cross-fades into the **previous**
  frame's generation using the square of the MDCT window, then the previous
  generation runs constantly to the end of the segment.
* Long frames (`LM != 0`, 5/10/20 ms): a second segment cross-fades the
  previous generation into the **current** (freshly entropy-decoded)
  generation, which then runs constantly to the end of the frame.
* Short frames (`LM == 0`, 2.5 ms): `shortMdctSize == N`, so only the first
  segment exists; the two-generation fade covers the whole frame.

`tests/test_postfilter_decode.c` generates continuous, left/right-distinct
periodic signals with the public encoder at 2.5, 5 and 20 ms and decodes them
with the public decoder. The observation hook returns only the current decoded
parameters, the pre-filter synthesis samples and the post-filter samples; the
decoder's `comb_filter()` call arguments are never used as the expected
parameters. The test maintains its own two generations and per-channel history,
replays the timeline, and compares every low-amplitude sample against the
reference with the bound `4e-5 * (1 + |expected|)`. It also requires finite
output, untouched history and guard regions, and a nonzero post-filter in every
length. It reports the number of samples compared per packet length and the
packet, channel and sample index of the first difference.

## Degeneracy checks

`check_postfilter.sh` edits a temporary copy of the sources in two ways that
still compile, and requires the content assertions to fail with an explicit
`POSTFILTER_*_FAIL` marker and a non-zero exit (a crash or a build failure does
not count):

* square cross-fade replaced by a linear cross-fade;
* the first segment's two generations merged early (so short packets apply the
  fresh parameters immediately).
