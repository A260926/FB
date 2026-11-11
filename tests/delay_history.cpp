/*
 * delay_history.cpp -- temporal history regression tests for delay0r.
 *
 * The plugin is driven exclusively through its public f0r_* ABI (the C++
 * frei0r.hpp wrapper exports).  Expected frames come from tests/delay_ref.cpp,
 * an independent time-stamped image list that never reads production state or
 * reuses the plugin's selection logic.
 *
 * Every source frame uses a distinct, index-dependent pixel pattern so the
 * selected frame's origin is identifiable; the host input buffer is
 * overwritten after every call, proving the plugin retains its own copy.
 * Both separate output buffers and in-place input/output overlap are tested.
 */
extern "C" {
#include "frei0r.h"
}

#include "delay_ref.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#define WIDTH 2u
#define HEIGHT 1u
#define NPIX (WIDTH * HEIGHT)
#define BYTES (NPIX * 4u)

static int failures;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                        \
    }                                                                    \
  } while (0)

/* Frame id k produces pixels whose high byte encodes k and whose low bytes
 * vary with the pixel index; any two ids are distinguishable bytewise. */
static uint32_t
pattern_pixel(unsigned int id, unsigned int index)
{
  return 0x80000000u | (id << 16) | ((id * 31u + index * 7u) & 0xFFFFu);
}

static void
fill_pattern(uint32_t* frame, unsigned int id)
{
  for (unsigned int i = 0; i < NPIX; i++)
    frame[i] = pattern_pixel(id, i);
}

static void
fill_garbage(uint32_t* frame)
{
  for (unsigned int i = 0; i < NPIX; i++)
    frame[i] = 0xDEAD0000u | (i * 0x101u);
}

static int
frames_equal(const uint32_t* a, const std::vector<uint32_t>& b)
{
  for (unsigned int i = 0; i < NPIX; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

static int
frames_equal_id(const uint32_t* frame, unsigned int id)
{
  for (unsigned int i = 0; i < NPIX; i++)
    if (frame[i] != pattern_pixel(id, i)) return 0;
  return 1;
}

static void
set_delay(f0r_instance_t s, DelayRef& ref, double seconds)
{
  f0r_set_param_value(s, &seconds, 0);
  ref.set_delay(seconds);
}

/* One plugin/reference update, in both buffer modes, with host overwrite. */
static void
check_update(f0r_instance_t s, DelayRef& ref, double time, unsigned int id,
             int in_place)
{
  uint32_t work[NPIX];
  fill_pattern(work, id);

  bool standalone = ref.observe(time, work);

  if (in_place)
  {
    f0r_update(s, time, work, work);  // input and output overlap
    if (!frames_equal(work, ref.selected()))
    {
      std::printf("FAIL %s:%d: mismatch at time %g (in-place)\n",
                  __FILE__, __LINE__, time);
      failures++;
    }
    fill_garbage(work);  // plugin must not borrow this buffer later
  }
  else
  {
    uint32_t out[NPIX];
    f0r_update(s, time, work, out);
    fill_garbage(work);  // host reuses/overwrites its input immediately
    if (!frames_equal(out, ref.selected()))
    {
      std::printf("FAIL %s:%d: mismatch at time %g (separate buffers)\n",
                  __FILE__, __LINE__, time);
      failures++;
    }
  }

  (void)standalone;
}

/* The exact pre-fix baseline scenario with the corrected assertion: at
 * t=0.8 with a 0.4s delay the newest frame not later than t=0.4 is the one
 * stored at t=0.3 (id 33), never the current frame (id 88). */
static void
test_known_defect_scenario(void)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.4);

  static const double times[] = { 0.0, 0.3, 0.8 };
  static const unsigned int ids[] = { 11, 33, 88 };
  uint32_t in[NPIX], out[NPIX];
  for (int i = 0; i < 3; i++)
  {
    fill_pattern(in, ids[i]);
    ref.observe(times[i], in);
    f0r_update(s, times[i], in, out);
    fill_garbage(in);
    CHECK(frames_equal(out, ref.selected()));
  }
  CHECK(frames_equal_id(out, 33));
  CHECK(!frames_equal_id(out, 88));
  f0r_destruct(s);
}

/* Fixed delay on an evenly spaced stream; every selection matches the
 * reference, including the oldest-frame fallback while history is short. */
static void
test_fixed_delay(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.25);

  for (int k = 0; k < 12; k++)
    check_update(s, ref, 0.1 * k, (unsigned int)(k + 1), in_place);
  f0r_destruct(s);
}

/* Uneven intervals and the one-second boundary: all frames of the most
 * recent second stay available plus one boundary frame, even while the delay
 * is animated up to a full second. */
static void
test_uneven_and_one_second_boundary(int in_place)
{
  static const double times[] = {
    0.0, 0.2, 0.5, 0.9, 1.1, 1.35, 1.7, 2.0, 2.05
  };
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.05);

  for (int k = 0; k < (int)(sizeof(times) / sizeof(times[0])); k++)
  {
    // Animate the delay, including a full second near the end.
    double d = (k == 8) ? 1.0 : (k % 3 == 0 ? 0.3 : 0.05);
    set_delay(s, ref, d);
    check_update(s, ref, times[k], (unsigned int)(k + 1), in_place);
  }

  // Direct boundary checks at t=2.05, delay=1.0: target is 1.05, the newest
  // frame not later than that is t=0.9 (id 4); the t=0..0.5 frames have been
  // reclaimed.
  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 42);
    ref.observe(2.05, in);
    f0r_update(s, 2.05, in, out);
    CHECK(frames_equal_id(out, 4));
  }
  // At t=1.35 with a full-second delay the target is 0.35; the newest frame
  // not later than that is t=0.2 (id 2), which must still be retained.
  {
    DelayRef r2(WIDTH, HEIGHT);
    f0r_instance_t s2 = f0r_construct(WIDTH, HEIGHT);
    set_delay(s2, r2, 1.0);
    uint32_t out2[NPIX];
    for (int k = 0; k < 6; k++)
    {
      uint32_t in2[NPIX];
      fill_pattern(in2, (unsigned int)(k + 1));
      r2.observe(times[k], in2);
      f0r_update(s2, times[k], in2, out2);
      fill_garbage(in2);
    }
    CHECK(frames_equal_id(out2, 2));  // t=0.2 is the boundary support frame
    f0r_destruct(s2);
  }
  f0r_destruct(s);
}

/* Start with zero delay (current frame always output), then grow the delay:
 * frames accumulated at zero delay must still be available. */
static void
test_zero_delay_then_increase(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.0);

  check_update(s, ref, 0.0, 101, in_place);
  check_update(s, ref, 0.1, 102, in_place);
  check_update(s, ref, 0.2, 103, in_place);

  set_delay(s, ref, 0.15);  // target 0.05 at t=0.2 -> frame t=0.0, id 101
  check_update(s, ref, 0.3, 104, in_place);

  // Explicit check of the delayed selection after the increase.
  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 105);
    ref.observe(0.4, in);
    f0r_update(s, 0.4, in, out);  // target 0.25 -> newest <= 0.25 is t=0.2
    CHECK(frames_equal_id(out, 103));
    fill_garbage(in);
  }
  f0r_destruct(s);
}

/* Same-time redraw replaces the stored frame; advancing afterwards must not
 * resurrect the superseded pixels. */
static void
test_same_time_redraw(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.0);

  check_update(s, ref, 1.0, 201, in_place);
  check_update(s, ref, 1.0, 202, in_place);  // redraw: replaces id 201

  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 202);
    ref.observe(1.0, in);
    f0r_update(s, 1.0, in, out);
    CHECK(frames_equal_id(out, 202));
    fill_garbage(in);
  }

  set_delay(s, ref, 0.5);  // at t=1.5 target 1.0 -> the replaced frame
  check_update(s, ref, 1.5, 203, in_place);
  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 204);
    ref.observe(1.6, in);
    f0r_update(s, 1.6, in, out);  // target 1.1 -> still frame t=1.0, id 202
    CHECK(frames_equal_id(out, 202));
    fill_garbage(in);
  }
  f0r_destruct(s);
}

/* A time smaller than the previous one starts a new playback segment and
 * drops all earlier frames; the oldest-frame fallback uses the new segment. */
static void
test_backwards_time(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.2);

  check_update(s, ref, 0.0, 301, in_place);
  check_update(s, ref, 1.0, 302, in_place);
  check_update(s, ref, 2.0, 303, in_place);

  set_delay(s, ref, 1.0);  // target would predate the new, reset segment
  check_update(s, ref, 0.5, 304, in_place);  // backwards: reset, fallback 304
  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 304);
    ref.observe(0.5, in);
    f0r_update(s, 0.5, in, out);
    CHECK(frames_equal_id(out, 304));
    fill_garbage(in);
  }
  check_update(s, ref, 0.7, 305, in_place);  // target -0.3 -> still oldest 304
  f0r_destruct(s);
}

/* Non-finite times are standalone: the input is output directly, nothing is
 * retained, and the next finite time starts a fresh history. */
static void
test_non_finite_time(int in_place)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.3);

  check_update(s, ref, 0.0, 401, in_place);
  check_update(s, ref, 0.2, 402, in_place);

  check_update(s, ref, NAN, 403, in_place);
  check_update(s, ref, INFINITY, 404, in_place);
  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 404);
    ref.observe(INFINITY, in);
    f0r_update(s, INFINITY, in, out);
    CHECK(frames_equal_id(out, 404));  // standalone passthrough
    fill_garbage(in);
  }

  set_delay(s, ref, 0.5);  // history is empty: oldest == current frame
  check_update(s, ref, 10.0, 405, in_place);
  {
    uint32_t in[NPIX], out[NPIX];
    fill_pattern(in, 405);
    ref.observe(10.0, in);
    f0r_update(s, 10.0, in, out);
    CHECK(frames_equal_id(out, 405));
    fill_garbage(in);
  }
  f0r_destruct(s);
}

/* Two instances interleaved with different delays, one in-place and one with
 * separate buffers, must keep fully independent frame stores. */
static void
test_interleaved_instances(void)
{
  f0r_instance_t a = f0r_construct(WIDTH, HEIGHT);
  f0r_instance_t b = f0r_construct(WIDTH, HEIGHT);
  DelayRef ra(WIDTH, HEIGHT), rb(WIDTH, HEIGHT);
  CHECK(a != 0 && b != 0);
  set_delay(a, ra, 0.2);
  set_delay(b, rb, 0.45);

  for (int k = 0; k < 15; k++)
  {
    check_update(a, ra, 0.1 * k, (unsigned int)(500 + k), 1);
    check_update(b, rb, 0.07 * k, (unsigned int)(700 + k), 0);
  }
  f0r_destruct(a);
  f0r_destruct(b);
}

/* A freshly constructed instance has no history: any delay falls back to its
 * first frame; destruction must release all retained copies. */
static void
test_construct_destruct_reuse(void)
{
  f0r_instance_t s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref, 0.9);
  check_update(s, ref, 0.0, 601, 0);
  check_update(s, ref, 0.3, 602, 1);
  f0r_destruct(s);

  s = f0r_construct(WIDTH, HEIGHT);
  DelayRef ref2(WIDTH, HEIGHT);
  CHECK(s != 0);
  set_delay(s, ref2, 0.9);
  uint32_t in[NPIX], out[NPIX];
  fill_pattern(in, 603);
  ref2.observe(0.0, in);
  f0r_update(s, 0.0, in, out);
  CHECK(frames_equal_id(out, 603));  // no frames carried over
  fill_garbage(in);
  f0r_destruct(s);
}

int
main()
{
  CHECK(f0r_init());

  test_known_defect_scenario();
  test_fixed_delay(0);
  test_fixed_delay(1);
  test_uneven_and_one_second_boundary(0);
  test_uneven_and_one_second_boundary(1);
  test_zero_delay_then_increase(0);
  test_zero_delay_then_increase(1);
  test_same_time_redraw(0);
  test_same_time_redraw(1);
  test_backwards_time(0);
  test_backwards_time(1);
  test_non_finite_time(0);
  test_non_finite_time(1);
  test_interleaved_instances();
  test_construct_destruct_reuse();

  f0r_deinit();
  if (failures == 0)
  {
    std::puts("delay0r temporal history: ok");
    return 0;
  }
  std::printf("delay0r temporal history: %d failure(s)\n", failures);
  return 1;
}
