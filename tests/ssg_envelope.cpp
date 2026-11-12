// SSG-EG regression tests for the OPN/OPNA FM core.
//
// This file is test-only code: it includes the OPN implementation directly in
// order to instantiate fm_engine_base<opna_registers> and the YM2612 chip, the
// same way baseline/smoke.cpp does. It does not modify any production source,
// does not rely on private access (only the public debug_* getters, the
// keyonoff/prepare/clock path and the public save/restore interface are used),
// and it contains no test-only branches that are compiled into the library.
//
// The expected values come from an independent SSG-EG event model implemented
// below (see model_held and the per-case expectation tables), not from
// recordings of the emulator's output.
//
// Build: see tests/run_ssg_envelope.sh

#include "ymfm_opn.cpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ymfm;

namespace
{

//-------------------------------------------------
// Test accounting
//-------------------------------------------------

int g_checks = 0;
int g_failures = 0;
int g_groups_run = 0;
bool g_group_seen[6] = { false, false, false, false, false, false };

struct ctx
{
	const char *group;
	unsigned mode;
	unsigned event;
	unsigned clock;
};

void report_fail(const ctx &c, const char *field, long expected, long actual)
{
	g_failures++;
	std::printf("FAIL group=%s mode=%u event#%u clock=%u field=%s expected=%ld(0x%lx) actual=%ld(0x%lx)\n",
		c.group, c.mode, c.event, c.clock, field,
		expected, static_cast<unsigned long>(expected & 0xffff),
		actual, static_cast<unsigned long>(actual & 0xffff));
}

void check(const ctx &c, const char *field, long expected, long actual)
{
	g_checks++;
	if (expected != actual)
		report_fail(c, field, expected, actual);
}

void group_begin(unsigned index, const char *name)
{
	g_groups_run++;
	g_group_seen[index] = true;
	std::printf("GROUP %s ...\n", name);
}

void group_end(const char *name, int checks_before)
{
	std::printf("PASS %s checks=%d\n", name, g_checks - checks_before);
}

//-------------------------------------------------
// Fixed register scenario
//
// Channel 0, operator offset 0. Raw rates are attack=31, decay=30,
// sustain=30, release=15. With block/fnum 0x0c00 the OPN keycode is 0, so the
// KSR adjustment (keycode >> (ksr ^ 3)) is 0 and the effective rates come out
// as: attack 62, decay 60, sustain 60, release 4*15+2 = 62.
//
// At effective rate 60-63 the attenuation_increment() table returns a
// constant 8. SSG-EG stages apply 4x that (32) while the raw attenuation is
// below 0x200; attack rate 62 snaps the attenuation to 0 on key-on.
//
// block/fnum 0x0c00 also gives a phase step of exactly 1024 in the 10.10
// phase representation, so the public phase() getter advances by 1 per clock,
// which makes the "phase reset then still step once" behaviour observable.
//-------------------------------------------------

constexpr uint32_t BLOCK_FREQ = 0x0c00;
constexpr uint32_t PHASE_STEP = 1024;
constexpr uint32_t ENV_TICK_PERIOD = 3;     // EG_CLOCK_DIVIDER=3 -> tick every 3 clocks
constexpr uint32_t SSG_INCREMENT = 0x20;    // 4 * 8
constexpr uint32_t PLAIN_INCREMENT = 0x08;  // non-SSG increment at rate 60
constexpr uint32_t MIDPOINT = 0x200;
constexpr uint32_t CYCLE_CLOCKS = 48;       // 16 SSG increments of 0x20
constexpr uint32_t SSG_EVENT_CLOCK = 49;    // detection runs before the envelope update

void write_op_setup(fm_engine_base<opna_registers> &fm, uint8_t ssg_reg)
{
	fm.write(0x30, 0x01); // detune 0, multiple 1
	fm.write(0x40, 0x00); // total level 0
	fm.write(0x50, 0x1f); // KSR 0, attack rate 31
	fm.write(0x60, 0x1e); // decay rate 30
	fm.write(0x70, 0x1e); // sustain rate 30
	fm.write(0x80, 0xff); // sustain level 15, release rate 15
	fm.write(0x90, ssg_reg);
	fm.write(0xa4, (BLOCK_FREQ >> 8) & 0x3f);
	fm.write(0xa0, BLOCK_FREQ & 0xff);
	fm.write(0xb0, 0x00); // feedback 0, algorithm 0
	fm.write(0xb4, 0xc0); // both outputs enabled
}

void key_on(fm_engine_base<opna_registers> &fm)
{
	fm.write(0x28, 0xf0); // channel 0, all four operator bits
}

void key_off(fm_engine_base<opna_registers> &fm)
{
	fm.write(0x28, 0x00);
}

fm_operator<opna_registers> *op0(fm_engine_base<opna_registers> &fm)
{
	return fm.debug_operator(0);
}

// effective attenuation as presented to the volume calculation; the production
// formula is (0x200 - raw) & 0x3ff when the SSG inversion flag is set
uint32_t effective_attenuation(uint32_t raw, uint32_t inverted)
{
	return inverted ? ((MIDPOINT - raw) & 0x3ff) : raw;
}

//-------------------------------------------------
// Independent SSG-EG event table for the eight modes, derived from the mode
// bit meanings alone (bit0 = hold, bit1 = alternate, bit2 = inverted start),
// plus the fixed-rate timing above. Nothing here reads production state.
//-------------------------------------------------

struct shape_info
{
	bool hold;            // bit 0: single run then hold
	bool alternate;       // bit 1: inversion toggles each cycle
	bool start_inverted;  // bit 2: inverted from key-on
	bool resets_phase;    // continuous, non-alternating modes reset phase
	uint8_t hold_polarity; // inversion flag at the held end (bit2 ^ bit1)
};

const shape_info k_shapes[8] =
{
	// mode  hold  alt  inv0  phase-reset  hold polarity
	{  false, false, false, true,  0 }, // 0: repeat normally
	{  true,  false, false, false, 0 }, // 1: run once, hold low
	{  false, true,  false, false, 0 }, // 2: repeat, alternating
	{  true,  true,  false, false, 1 }, // 3: run once, hold high
	{  false, false, true,  true,  1 }, // 4: inverted repeat normally
	{  true,  false, true,  false, 1 }, // 5: inverted run once, hold high
	{  false, true,  true,  false, 0 }, // 6: inverted repeat, alternating
	{  true,  true,  true,  false, 0 }, // 7: inverted run once, hold low
};

struct model_state
{
	uint32_t eg_state;
	uint32_t raw;
	uint32_t inverted;
	uint32_t phase;
	uint32_t effective;
};

// expected state of a held key-on note at the given clock
model_state model_held(unsigned mode, unsigned clock)
{
	const shape_info &s = k_shapes[mode];
	model_state m;

	// phase: free running 10.10 accumulation, reset to 0 at every SSG event
	// in modes 0/4; the reset is followed by this clock's own phase step
	uint32_t phase = 0;
	for (unsigned t = 1; t <= clock; ++t)
	{
		if (s.resets_phase && t >= SSG_EVENT_CLOCK && (t - SSG_EVENT_CLOCK) % CYCLE_CLOCKS == 0)
			phase = 0;
		phase += PHASE_STEP;
	}
	m.phase = phase >> 10;

	// clocks 1-2 are still in the rate-62 attack snap
	if (clock <= 2)
	{
		m.eg_state = EG_ATTACK;
		m.raw = 0;
		m.inverted = s.start_inverted;
		m.effective = effective_attenuation(m.raw, m.inverted);
		return m;
	}

	if (s.hold)
	{
		// single ramp up to clock 48, then a forced held end state
		if (clock < SSG_EVENT_CLOCK)
		{
			m.eg_state = EG_DECAY;
			m.raw = SSG_INCREMENT * (clock / ENV_TICK_PERIOD);
			m.inverted = s.start_inverted;
		}
		else
		{
			m.inverted = s.hold_polarity;
			if (s.hold_polarity == 0)
			{
				m.raw = 0x3ff;
				m.eg_state = (clock >= 51) ? EG_SUSTAIN : EG_DECAY;
			}
			else
			{
				m.raw = MIDPOINT;
				m.eg_state = EG_DECAY;
			}
		}
	}
	else
	{
		// continuous cycles of 48 clocks; clock 49/97/... is the SSG event
		unsigned cycle = (clock - 1) / CYCLE_CLOCKS;
		unsigned pos = (clock - 1) % CYCLE_CLOCKS;
		if ((cycle > 0 && pos == 0) || pos == 1)
		{
			// SSG event clock, or the attack clock one later
			m.eg_state = EG_ATTACK;
			m.raw = 0;
		}
		else
		{
			m.eg_state = EG_DECAY;
			unsigned local = pos + 1;
			m.raw = SSG_INCREMENT * (local / ENV_TICK_PERIOD);
		}
		m.inverted = s.start_inverted;
		if (s.alternate && clock >= SSG_EVENT_CLOCK)
		{
			unsigned events = (clock - SSG_EVENT_CLOCK) / CYCLE_CLOCKS + 1;
			m.inverted ^= (events & 1);
		}
	}

	m.effective = effective_attenuation(m.raw, m.inverted);
	return m;
}

//-------------------------------------------------
// Group 1: full trajectory of all eight shapes
//-------------------------------------------------

void test_shape_trajectories()
{
	const int before = g_checks;
	group_begin(0, "shape_trajectories");
	constexpr unsigned DURATION = 150; // three midpoint boundaries (48/96/144)

	for (unsigned mode = 0; mode < 8; ++mode)
	{
		ymfm_interface io;
		fm_engine_base<opna_registers> fm(io);
		fm.reset();
		write_op_setup(fm, uint8_t(0x08 | mode));
		key_on(fm);

		// one prepare/clock to populate the cache, then validate fixed rates
		fm.clock(1);
		auto &cache = op0(fm)->debug_cache();
		ctx c = { "shape_trajectories", mode, 0, 1 };
		check(c, "attack_rate", 62, cache.eg_rate[EG_ATTACK]);
		check(c, "decay_rate", 60, cache.eg_rate[EG_DECAY]);
		check(c, "sustain_rate", 60, cache.eg_rate[EG_SUSTAIN]);
		check(c, "release_rate", 62, cache.eg_rate[EG_RELEASE]);
		check(c, "eg_shift", 0, cache.eg_shift);
		check(c, "phase_step", PHASE_STEP, cache.phase_step);

		for (unsigned t = 1; t <= DURATION; ++t)
		{
			// clock 1 already happened above
			if (t > 1)
				fm.clock(1);
			auto *op = op0(fm);
			model_state want = model_held(mode, t);
			ctx at = { "shape_trajectories", mode, t, t };
			check(at, "eg_state", want.eg_state, op->debug_eg_state());
			check(at, "raw_attenuation", want.raw, op->debug_eg_attenuation());
			check(at, "ssg_inverted", want.inverted, op->debug_ssg_inverted());
			check(at, "phase", want.phase, op->phase());
			check(at, "effective_attenuation", want.effective,
				effective_attenuation(op->debug_eg_attenuation(), op->debug_ssg_inverted()));
		}

		// explicit boundary accounting: continuous modes show an SSG-built
		// attack at clocks 49, 97 and 145; hold modes settle once
		auto *op = op0(fm);
		ctx end = { "shape_trajectories", mode, 999, DURATION };
		if (k_shapes[mode].hold)
		{
			check(end, "hold_settled", 1,
				(op->debug_eg_state() == EG_DECAY || op->debug_eg_state() == EG_SUSTAIN) ? 1 : 0);
		}
		else
		{
			// the third SSG event (clock 145) must rebuild the attack again
			model_state third_event = model_held(mode, 145);
			check(end, "third_event_is_attack", EG_ATTACK, third_event.eg_state);
			check(end, "third_event_raw_resets", 0, third_event.raw);
		}
	}
	group_end("shape_trajectories", before);
}

//-------------------------------------------------
// Group 2: the 0x1ff vs exactly-0x200 distinction, built through the public
// save/restore interface. Every constructed state is read back before use,
// and group 1 provides the continuously-run counterpart of the same boundary.
//-------------------------------------------------

// locate operator 0's attenuation field inside a saved-state buffer by its
// known neighbours: uint16 attenuation, uint8 envelope state, uint8 inversion
size_t find_attenuation_offset(const std::vector<uint8_t> &buffer, uint16_t atten, uint8_t state)
{
	size_t found = ~size_t(0);
	for (size_t i = 0; i + 6 < buffer.size(); ++i)
	{
		if (uint8_t(buffer[i]) == (atten & 0xff) &&
			uint8_t(buffer[i + 1]) == (atten >> 8) &&
			buffer[i + 2] == state)
		{
			if (found != ~size_t(0))
				return ~size_t(0); // not unique, refuse to guess
			found = i;
		}
	}
	return found;
}

void patch_operator_state(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst,
	uint16_t atten, uint8_t eg_state, uint8_t inverted, size_t atten_off)
{
	dst = src;
	dst[atten_off + 0] = atten & 0xff;
	dst[atten_off + 1] = atten >> 8;
	dst[atten_off + 2] = eg_state;
	dst[atten_off + 3] = inverted;
}

void test_constructed_boundary()
{
	const int before = g_checks;
	group_begin(1, "constructed_boundary");

	// run a real note to a known decay point (raw 0x1e0 at clock 45) and save
	ymfm_interface io_runner;
	fm_engine_base<opna_registers> runner(io_runner);
	runner.reset();
	write_op_setup(runner, 0x08); // mode 0
	key_on(runner);
	for (unsigned t = 0; t < 45; ++t)
		runner.clock(1);
	ctx c = { "constructed_boundary", 0, 0, 45 };
	check(c, "seed_raw", 0x1e0, op0(runner)->debug_eg_attenuation());
	check(c, "seed_state", EG_DECAY, op0(runner)->debug_eg_state());

	std::vector<uint8_t> saved;
	{
		ymfm_saved_state state(saved, true);
		runner.save_restore(state);
	}
	size_t atten_off = find_attenuation_offset(saved, 0x1e0, uint8_t(EG_DECAY));
	check(c, "attenuation_offset_found", 1, atten_off != ~size_t(0) ? 1 : 0);
	if (atten_off == ~size_t(0))
	{
		group_end("constructed_boundary", before);
		return;
	}

	// ---- constructed 0x1ff: midpoint bit is clear, no SSG action yet ----
	{
		std::vector<uint8_t> buf;
		patch_operator_state(saved, buf, 0x1ff, uint8_t(EG_DECAY), 0, atten_off);
		ymfm_interface io;
		fm_engine_base<opna_registers> fm(io);
		{
			ymfm_saved_state state(buf, false);
			fm.save_restore(state);
		}
		auto *op = op0(fm);
		ctx r = { "constructed_boundary", 0, 1, 45 };
		check(r, "readback_raw_1ff", 0x1ff, op->debug_eg_attenuation());
		check(r, "readback_state", EG_DECAY, op->debug_eg_state());
		check(r, "readback_inverted", 0, op->debug_ssg_inverted());

		// clocks 46 and 47 are not envelope ticks: nothing moves
		fm.clock(1);
		ctx a1 = { "constructed_boundary", 0, 2, 46 };
		check(a1, "tick46_raw", 0x1ff, op->debug_eg_attenuation());
		check(a1, "tick46_state", EG_DECAY, op->debug_eg_state());
		fm.clock(1);
		ctx a2 = { "constructed_boundary", 0, 3, 47 };
		check(a2, "tick47_raw", 0x1ff, op->debug_eg_attenuation());

		// clock 48 is an envelope tick; detection still saw bit9 clear before
		// the update, so the 4x increment lands at 0x21f with no SSG event
		fm.clock(1);
		ctx a3 = { "constructed_boundary", 0, 4, 48 };
		check(a3, "tick48_crosses_without_event", 0x21f, op->debug_eg_attenuation());
		check(a3, "tick48_state", EG_DECAY, op->debug_eg_state());
		check(a3, "tick48_inverted", 0, op->debug_ssg_inverted());

		// clock 49 now sees bit9 set and fires the event one clock later than
		// the exactly-0x200 construction below
		fm.clock(1);
		ctx a4 = { "constructed_boundary", 0, 5, 49 };
		check(a4, "delayed_event_state", EG_ATTACK, op->debug_eg_state());
		check(a4, "delayed_event_raw", 0, op->debug_eg_attenuation());
		check(a4, "delayed_event_phase_step_taken", 1, op->phase());
	}

	// ---- constructed exactly 0x200: detection fires before the update ----
	{
		std::vector<uint8_t> buf;
		patch_operator_state(saved, buf, 0x200, uint8_t(EG_DECAY), 0, atten_off);
		ymfm_interface io;
		fm_engine_base<opna_registers> fm(io);
		{
			ymfm_saved_state state(buf, false);
			fm.save_restore(state);
		}
		auto *op = op0(fm);
		ctx r = { "constructed_boundary", 0, 6, 45 };
		check(r, "readback_raw_200", 0x200, op->debug_eg_attenuation());
		check(r, "readback_state", EG_DECAY, op->debug_eg_state());

		// one clock: SSG event restarts the attack and resets the phase, but
		// this clock's phase step is still taken (0 + 1024 -> phase() == 1)
		fm.clock(1);
		ctx a = { "constructed_boundary", 0, 7, 46 };
		check(a, "event_state", EG_ATTACK, op->debug_eg_state());
		check(a, "event_raw", 0, op->debug_eg_attenuation());
		check(a, "event_inverted", 0, op->debug_ssg_inverted());
		check(a, "event_phase_step_taken", 1, op->phase());
	}
	group_end("constructed_boundary", before);
}

//-------------------------------------------------
// Group 3: key-off into release, and re-key-on
//-------------------------------------------------

const uint32_t DONT_CARE = 999;

struct expectation
{
	uint32_t eg_state;
	uint32_t raw;
	uint32_t inverted;
	uint32_t phase;
};

void run_release_case(const char *name, unsigned mode, unsigned keyoff_clock,
	unsigned reon_clock, unsigned duration,
	const std::vector<expectation> &wants)
{
	ymfm_interface io;
	fm_engine_base<opna_registers> fm(io);
	fm.reset();
	write_op_setup(fm, uint8_t(0x08 | mode));
	key_on(fm);

	unsigned event_no = 0;
	for (unsigned t = 1; t <= duration; ++t)
	{
		if (t == keyoff_clock)
			key_off(fm);
		if (reon_clock != 0 && t == reon_clock)
			key_on(fm);
		fm.clock(1);

		const expectation &want = wants[t - 1];
		if (want.eg_state == DONT_CARE)
			continue;
		auto *op = op0(fm);
		ctx at = { name, mode, ++event_no, t };
		check(at, "eg_state", want.eg_state, op->debug_eg_state());
		check(at, "raw_attenuation", want.raw, op->debug_eg_attenuation());
		check(at, "ssg_inverted", want.inverted, op->debug_ssg_inverted());
		check(at, "effective_attenuation", effective_attenuation(want.raw, want.inverted),
			effective_attenuation(op->debug_eg_attenuation(), op->debug_ssg_inverted()));
		if (want.phase != DONT_CARE)
			check(at, "phase", want.phase, op->phase());
	}
}

std::vector<expectation> make_expectations(unsigned duration)
{
	return std::vector<expectation>(duration, { DONT_CARE, DONT_CARE, DONT_CARE, DONT_CARE });
}

void want_at(std::vector<expectation> &v, unsigned clock,
	uint32_t state, uint32_t raw, uint32_t inverted, uint32_t phase)
{
	v[clock - 1] = { state, raw, inverted, phase };
}

void test_release_behavior()
{
	const int before = g_checks;
	group_begin(2, "release_behavior");

	// case A: key-off during a non-inverted decay (mode 0, clock 30). Release
	// keeps the 4x SSG increment below the midpoint; at 0x3ff the mode-0 phase
	// reset has already fired at clock 49, and phase then freezes at 1
	{
		const unsigned dur = 52;
		auto e = make_expectations(dur);
		want_at(e, 29, EG_DECAY, 0x120, 0, 29); // last decay clock before key-off
		want_at(e, 30, EG_RELEASE, 0x140, 0, 30); // same ramp continues, no jump
		want_at(e, 33, EG_RELEASE, 0x160, 0, 33); // release still adds 0x20/tick
		want_at(e, 45, EG_RELEASE, 0x1e0, 0, 45);
		want_at(e, 48, EG_RELEASE, 0x200, 0, 48); // reaches the midpoint
		want_at(e, 49, EG_RELEASE, 0x3ff, 0, 1);  // forced max, phase reset + step
		want_at(e, 51, EG_RELEASE, 0x3ff, 0, 1);  // quiet operator, phase frozen
		run_release_case("release/non-inverted-decay", 0, 30, 0, dur, e);
	}

	// case B: key-off during an inverted decay (mode 2, clock 60). The
	// inverted raw value is snapped so the effective envelope continues
	// without a jump (pre-keyoff effective 0x1a0, then the same-clock tick),
	// and the inversion flag is cleared
	{
		const unsigned dur = 67;
		auto e = make_expectations(dur);
		// one clock before key-off: raw 0x60 inverted means effective 0x1a0;
		// start_release() must snap the release raw value to that same 0x1a0
		want_at(e, 59, EG_DECAY, 0x060, 1, 59);
		want_at(e, 60, EG_RELEASE, 0x1c0, 0, 60); // snap 0x200-0x60, then +0x20
		want_at(e, 63, EG_RELEASE, 0x1e0, 0, 63);
		want_at(e, 66, EG_RELEASE, 0x200, 0, 66);
		want_at(e, 67, EG_RELEASE, 0x3ff, 1, 67); // alternating toggle + force max
		run_release_case("release/inverted-decay-snap", 2, 60, 0, dur, e);
	}

	// case C: past the midpoint in release, the alternating mode keeps
	// toggling the inversion flag every clock while raw is held at 0x3ff
	{
		const unsigned dur = 112;
		auto e = make_expectations(dur);
		want_at(e, 102, EG_RELEASE, 0x3ff, 0, 102);
		want_at(e, 103, EG_RELEASE, 0x3ff, 1, 103);
		want_at(e, 105, EG_RELEASE, 0x3ff, 1, 105);
		want_at(e, 108, EG_RELEASE, 0x3ff, 0, 108);
		want_at(e, 109, EG_RELEASE, 0x3ff, 1, 109);
		want_at(e, 111, EG_RELEASE, 0x3ff, 1, 111);
		run_release_case("release/inverted-midpoint", 2, 60, 0, dur, e);
	}

	// case D: key-off at the held-high end (mode 3 holds raw 0x200 inverted,
	// effective 0); release starts from raw 0, keeping effective continuity
	{
		const unsigned dur = 78;
		auto e = make_expectations(dur);
		// held-high end: raw 0x200 inverted is effective 0, and the release
		// snap lands on raw 0 so the effective envelope stays at 0
		want_at(e, 69, EG_DECAY, 0x200, 1, 69);
		want_at(e, 70, EG_RELEASE, 0x000, 0, 70);
		want_at(e, 72, EG_RELEASE, 0x020, 0, 72);
		want_at(e, 75, EG_RELEASE, 0x040, 0, 75);
		run_release_case("release/hold-high", 3, 70, 0, dur, e);
	}

	// case E: key-off at the held-low end (mode 1 sits at 0x3ff in sustain);
	// release simply stays at maximum
	{
		const unsigned dur = 62;
		auto e = make_expectations(dur);
		want_at(e, 55, EG_SUSTAIN, 0x3ff, 0, 55); // one clock before key-off
		want_at(e, 56, EG_RELEASE, 0x3ff, 0, 56);
		want_at(e, 60, EG_RELEASE, 0x3ff, 0, 60);
		run_release_case("release/hold-low", 1, 56, 0, dur, e);
	}

	// case F: key-off one clock before the midpoint vs exactly on it
	{
		const unsigned dur = 52;
		auto e = make_expectations(dur);
		want_at(e, 47, EG_RELEASE, 0x1e0, 0, 47);
		want_at(e, 48, EG_RELEASE, 0x200, 0, 48); // SSG 4x increment still applied
		want_at(e, 49, EG_RELEASE, 0x3ff, 0, 1);  // phase reset still fires
		run_release_case("release/before-midpoint", 0, 47, 0, dur, e);
	}
	{
		const unsigned dur = 52;
		auto e = make_expectations(dur);
		want_at(e, 48, EG_RELEASE, 0x200, 0, 48); // no increment at/over midpoint
		want_at(e, 49, EG_RELEASE, 0x3ff, 0, 1);
		run_release_case("release/at-midpoint", 0, 48, 0, dur, e);
	}

	// case G: re-key-on rebuilds attack, inversion and phase from scratch;
	// the next SSG event 48 clocks later behaves per the (alternating) shape
	{
		const unsigned dur = 132;
		auto e = make_expectations(dur);
		want_at(e, 78, EG_RELEASE, 0x3ff, 0, 78);
		want_at(e, 80, EG_ATTACK, 0x000, 0, 1);  // phase reset, then step
		want_at(e, 81, EG_DECAY, 0x020, 0, 2);   // attack->decay, first tick same clock
		want_at(e, 83, EG_DECAY, 0x020, 0, 4);   // next SSG ramp tick lands at 84
		want_at(e, 127, EG_ATTACK, 0x000, 1, 48); // SSG event 47 clocks after re-key
		want_at(e, 128, EG_ATTACK, 0x000, 1, 49);
		want_at(e, 129, EG_DECAY, 0x020, 1, 50); // inverted ramp, no phase reset
		run_release_case("release/rekey", 2, 60, 80, dur, e);
	}

	group_end("release_behavior", before);
}

//-------------------------------------------------
// Group 4: live register changes are not key-ons; SSG disable/re-enable
// follows the existing clock() rules
//-------------------------------------------------

void test_dynamic_register_changes()
{
	const int before = g_checks;
	group_begin(3, "dynamic_register_changes");

	// switch shape 0 -> 2 while the note is held: no key-on, no phase reset
	{
		ymfm_interface io;
		fm_engine_base<opna_registers> fm(io);
		fm.reset();
		write_op_setup(fm, 0x08);
		key_on(fm);
		for (unsigned t = 1; t <= 110; ++t)
		{
			if (t == 30)
				fm.write(0x90, 0x0a); // shape 2, still enabled
			fm.clock(1);
			auto *op = op0(fm);
			if (t == 30)
			{
				ctx c = { "dynamic/shape-switch", 2, 1, t };
				check(c, "state_unchanged", EG_DECAY, op->debug_eg_state());
				check(c, "raw_unchanged", 0x140, op->debug_eg_attenuation());
				check(c, "phase_continues", 30, op->phase());
			}
			if (t == 49)
			{
				// mode 2 behaviour at the first event: invert, restart, no reset
				ctx c = { "dynamic/shape-switch", 2, 2, t };
				check(c, "event_state", EG_ATTACK, op->debug_eg_state());
				check(c, "event_inverted", 1, op->debug_ssg_inverted());
				check(c, "event_phase_not_reset", 49, op->phase());
			}
		}
	}

	// disable SSG at clock 20, re-enable at clock 40: increments drop to 8,
	// the inversion flag clears, then 4x SSG increments resume
	{
		ymfm_interface io;
		fm_engine_base<opna_registers> fm(io);
		fm.reset();
		write_op_setup(fm, 0x08);
		key_on(fm);
		for (unsigned t = 1; t <= 70; ++t)
		{
			if (t == 20)
				fm.write(0x90, 0x00); // disabled
			if (t == 40)
				fm.write(0x90, 0x08); // re-enabled, mode 0
			fm.clock(1);
			auto *op = op0(fm);
			if (t == 21)
			{
				ctx c = { "dynamic/disable-enable", 0, 1, t };
				check(c, "plain_increment", 0xc8, op->debug_eg_attenuation());
				check(c, "inversion_cleared", 0, op->debug_ssg_inverted());
			}
			if (t == 39)
			{
				ctx c = { "dynamic/disable-enable", 0, 2, t };
				check(c, "plain_ramp", 0xf8, op->debug_eg_attenuation());
			}
			if (t == 42)
			{
				ctx c = { "dynamic/disable-enable", 0, 3, t };
				check(c, "ssg_increment_resumed", 0x118, op->debug_eg_attenuation());
			}
			if (t == 66)
			{
				ctx c = { "dynamic/disable-enable", 0, 4, t };
				check(c, "crossed_midpoint_no_event", 0x218, op->debug_eg_attenuation());
				check(c, "still_decay", EG_DECAY, op->debug_eg_state());
			}
			if (t == 67)
			{
				ctx c = { "dynamic/disable-enable", 0, 5, t };
				check(c, "delayed_event_restart", EG_ATTACK, op->debug_eg_state());
				check(c, "delayed_event_raw", 0, op->debug_eg_attenuation());
				check(c, "delayed_event_phase", 1, op->phase());
			}
		}
	}
	group_end("dynamic_register_changes", before);
}

//-------------------------------------------------
// Group 5: SSG disabled, ordinary envelope control case
//-------------------------------------------------

void test_disabled_control()
{
	const int before = g_checks;
	group_begin(4, "ssg_disabled_control");

	ymfm_interface io;
	fm_engine_base<opna_registers> fm(io);
	fm.reset();
	write_op_setup(fm, 0x00); // SSG-EG disabled
	key_on(fm);

	for (unsigned t = 1; t <= 210; ++t)
	{
		fm.clock(1);
		auto *op = op0(fm);
		ctx at = { "ssg_disabled_control", 0, t, t };
		check(at, "never_inverted", 0, op->debug_ssg_inverted());
		if (t == 48)
		{
			// plain rate-60 increment: 16 ticks * 8 = 0x80, not the SSG 0x200
			check(at, "plain_raw_at_48", 16 * PLAIN_INCREMENT, op->debug_eg_attenuation());
			check(at, "still_decay", EG_DECAY, op->debug_eg_state());
		}
		if (t == 192)
		{
			// crossing the SSG midpoint value has no special meaning here
			check(at, "raw_passes_200", MIDPOINT, op->debug_eg_attenuation());
			check(at, "no_restart", EG_DECAY, op->debug_eg_state());
		}
		if (t == 195)
		{
			check(at, "raw_continues_past_200", MIDPOINT + PLAIN_INCREMENT, op->debug_eg_attenuation());
			check(at, "phase_free_running", 195, op->phase());
		}
	}
	group_end("ssg_disabled_control", before);
}

//-------------------------------------------------
// Group 6: the operator mechanisms must be wired into the real YM2612
// generate() path, with save/restore into a second chip instance and
// sample-exact chunked generation
//-------------------------------------------------

// Channel 0 of an OPNA maps its four slot operators (numbers 0, 6, 3, 9) to
// register offsets 0, 6, 8, 12. All four must be configured or the unconfigured
// operators stay at maximum attenuation and the channel only emits a DC level.
const unsigned kChannel0Offsets[4] = { 0, 6, 8, 12 };

// A faster block/fnum than the operator-level 0x0c00: here the phase sweeps
// across the sine table inside the window, so generate() produces a changing
// waveform instead of a near-constant value. The envelope assertions live in
// the other groups; this group only needs the wiring to be audible.
constexpr uint32_t AUDIO_BLOCK_FREQ = 0x3400;

void ym2612_setup(ym2612 &chip, uint8_t ssg_reg)
{
	chip.reset();
	for (unsigned off : kChannel0Offsets)
	{
		chip.write(0, 0x30 + off); chip.write(1, 0x01);
		chip.write(0, 0x40 + off); chip.write(1, 0x00);
		chip.write(0, 0x50 + off); chip.write(1, 0x1f);
		chip.write(0, 0x60 + off); chip.write(1, 0x1e);
		chip.write(0, 0x70 + off); chip.write(1, 0x1e);
		chip.write(0, 0x80 + off); chip.write(1, 0xff);
		chip.write(0, 0x90 + off); chip.write(1, ssg_reg);
	}
	chip.write(0, 0xa4); chip.write(1, (AUDIO_BLOCK_FREQ >> 8) & 0x3f);
	chip.write(0, 0xa0); chip.write(1, AUDIO_BLOCK_FREQ & 0xff);
	chip.write(0, 0xb0); chip.write(1, 0x00);
	chip.write(2, 0xb4); chip.write(3, 0xc0);
}

using pcm_buffer = std::vector<ym2612::output_data>;

bool pcm_equal(const pcm_buffer &a, const pcm_buffer &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		for (int ch = 0; ch < 2; ++ch)
			if (a[i].data[ch] != b[i].data[ch])
				return false;
	return true;
}

unsigned pcm_distinct_left(const pcm_buffer &a)
{
	std::vector<int32_t> values;
	for (auto &s : a)
	{
		bool seen = false;
		for (int32_t v : values)
			if (v == s.data[0]) { seen = true; break; }
		if (!seen)
			values.push_back(s.data[0]);
	}
	return static_cast<unsigned>(values.size());
}

void test_audio_path()
{
	const int before = g_checks;
	group_begin(5, "audio_generate_path");
	constexpr unsigned TOTAL = 200;
	constexpr unsigned MID = 72;
	const unsigned chunks[3] = { 1, 7, 127 };

	// ---- phase A: a continuously sounding note. Whole-buffer generate must
	// match 1/7/127 chunked generate sample-for-sample, and a real hand-off
	// must match while a chip that never received the state must not ----
	{
		ymfm_interface io_full, io_chunk;
		ym2612 full(io_full), chunked(io_chunk);
		ym2612_setup(full, 0x08);
		ym2612_setup(chunked, 0x08);
		full.write(0, 0x28); full.write(1, 0xf0);
		chunked.write(0, 0x28); chunked.write(1, 0xf0);

		pcm_buffer ref(TOTAL), got(TOTAL);
		full.generate(ref.data(), TOTAL);

		unsigned pos = 0, ci = 0;
		while (pos < TOTAL)
		{
			unsigned n = chunks[ci++ % 3];
			if (n > TOTAL - pos)
				n = TOTAL - pos;
			chunked.generate(got.data() + pos, n);
			pos += n;
		}

		ctx c = { "audio_generate_path", 0, 1, TOTAL };
		check(c, "chunked_sample_exact", 1, pcm_equal(ref, got) ? 1 : 0);
		unsigned nonzero_l = 0, nonzero_r = 0;
		for (auto &s : ref)
		{
			if (s.data[0] != 0) ++nonzero_l;
			if (s.data[1] != 0) ++nonzero_r;
		}
		check(c, "stereo_nonzero_left", TOTAL, nonzero_l);
		check(c, "stereo_nonzero_right", TOTAL, nonzero_r);
		// a changing waveform, not a flat DC offset, proves real synthesis
		check(c, "waveform_changes", 1, pcm_distinct_left(ref) >= 20 ? 1 : 0);

		// save the running chip and continue on a second instance
		ymfm_interface io_src, io_restored;
		ym2612 src(io_src), restored(io_restored);
		ym2612_setup(src, 0x08);
		ym2612_setup(restored, 0x08);
		src.write(0, 0x28); src.write(1, 0xf0);
		pcm_buffer handed(TOTAL);
		src.generate(handed.data(), MID);
		std::vector<uint8_t> saved;
		{
			ymfm_saved_state state(saved, true);
			src.save_restore(state);
		}
		{
			ymfm_saved_state state(saved, false);
			restored.save_restore(state);
		}
		restored.generate(handed.data() + MID, TOTAL - MID);
		check(c, "restore_sample_exact", 1, pcm_equal(ref, handed) ? 1 : 0);
		check(c, "save_buffer_nonempty", 1, saved.size() > 0 ? 1 : 0);

		// negative control: a fresh chip with no transferred state cannot
		// reproduce the audible tail, so the equality check has real teeth
		ymfm_interface io_nohand;
		ym2612 no_handover(io_nohand);
		ym2612_setup(no_handover, 0x08);
		pcm_buffer silent(TOTAL);
		no_handover.generate(silent.data() + MID, TOTAL - MID);
		unsigned tail_mismatch = 0;
		for (unsigned i = MID; i < TOTAL; ++i)
			if (ref[i].data[0] != silent[i].data[0])
				++tail_mismatch;
		check(c, "handoff_is_load_bearing", 1, tail_mismatch > 0 ? 1 : 0);
	}

	// ---- phase B: event timeline (key-off and a shape write at fixed
	// absolute sample positions) compared across a fine-step reference, a
	// 1/7/127 chunked run, and a run saved/restored into another chip ----
	{
		constexpr unsigned KEYOFF_AT = 30;
		constexpr unsigned SHAPE_AT = 70;

		auto apply_events = [](ym2612 &chip, unsigned from, unsigned to)
		{
			for (unsigned t = from; t < to; ++t)
			{
				if (t == KEYOFF_AT)
				{
					chip.write(0, 0x28); chip.write(1, 0x00);
				}
				if (t == SHAPE_AT)
				{
					// mode 0 -> 2 on every channel-0 operator; a register
					// write, not a key-on
					for (unsigned off : kChannel0Offsets)
					{
						chip.write(0, 0x90 + off);
						chip.write(1, 0x0a);
					}
				}
			}
		};

		// fine-step reference: one sample per generate call
		ymfm_interface io_ref;
		ym2612 ref(io_ref);
		ym2612_setup(ref, 0x08);
		ref.write(0, 0x28); ref.write(1, 0xf0);
		pcm_buffer reference(TOTAL);
		for (unsigned t = 0; t < TOTAL; ++t)
		{
			apply_events(ref, t, t + 1);
			ref.generate(reference.data() + t, 1);
		}

		// chunked run with the same absolute event positions
		ymfm_interface io_chunk;
		ym2612 chunked(io_chunk);
		ym2612_setup(chunked, 0x08);
		chunked.write(0, 0x28); chunked.write(1, 0xf0);
		pcm_buffer chunk_pcm(TOTAL);
		unsigned pos = 0, ci = 0;
		while (pos < TOTAL)
		{
			unsigned n = chunks[ci++ % 3];
			if (n > TOTAL - pos)
				n = TOTAL - pos;
			apply_events(chunked, pos, pos + n);
			chunked.generate(chunk_pcm.data() + pos, n);
			pos += n;
		}

		// separate source chip runs to MID, then hands its saved state to a
		// second chip instance which continues the timeline
		ymfm_interface io_src, io_restored;
		ym2612 src(io_src), restored(io_restored);
		ym2612_setup(src, 0x08);
		ym2612_setup(restored, 0x08);
		src.write(0, 0x28); src.write(1, 0xf0);
		pcm_buffer restored_pcm(TOTAL);
		for (unsigned t = 0; t < MID; ++t)
		{
			apply_events(src, t, t + 1);
			src.generate(restored_pcm.data() + t, 1);
		}
		std::vector<uint8_t> saved;
		{
			ymfm_saved_state state(saved, true);
			src.save_restore(state);
		}
		{
			ymfm_saved_state state(saved, false);
			restored.save_restore(state);
		}
		// register file and key state arrive in the saved blob; only apply
		// events at or after the hand-off point
		apply_events(restored, MID, TOTAL);
		restored.generate(restored_pcm.data() + MID, TOTAL - MID);

		ctx c = { "audio_generate_path", 0, 2, TOTAL };
		check(c, "event_timeline_chunked", 1, pcm_equal(reference, chunk_pcm) ? 1 : 0);
		check(c, "event_timeline_restored", 1, pcm_equal(reference, restored_pcm) ? 1 : 0);
	}
	group_end("audio_generate_path", before);
}

} // anonymous namespace

//-------------------------------------------------
// main
//-------------------------------------------------

int main()
{
	test_shape_trajectories();
	test_constructed_boundary();
	test_release_behavior();
	test_dynamic_register_changes();
	test_disabled_control();
	test_audio_path();

	std::printf("SUMMARY groups=%d checks=%d failures=%d\n",
		g_groups_run, g_checks, g_failures);
	if (g_groups_run != 6)
	{
		std::printf("FAIL missing test groups (ran %d of 6)\n", g_groups_run);
		return 2;
	}
	for (unsigned i = 0; i < 6; ++i)
		if (!g_group_seen[i])
		{
			std::printf("FAIL group %u was skipped\n", i);
			return 2;
		}
	if (g_checks < 200)
	{
		std::printf("FAIL too few assertions executed: %d\n", g_checks);
		return 2;
	}
	if (g_failures != 0)
		return 1;
	std::puts("SSG_ENVELOPE_ALL_PASS");
	return 0;
}
