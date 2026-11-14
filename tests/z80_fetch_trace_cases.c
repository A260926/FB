//------------------------------------------------------------------------------
//  z80_fetch_trace_cases.c
//
//  Self-checking tests for the optional instruction fetch-byte tracer in
//  chips/z80.h. Programs are plain C byte arrays run against a controlled,
//  bank-aware bus with injected WAIT states and interrupts.
//
//  Groups:
//    bytes       encoding sequences, roles, sources, DD CB, banks, wrap, ...
//    waits       WAIT insertion around read requests (no dup/early/stale src)
//    interrupts  IM0/IM1/IM2 and NMI, including HALT wake-up
//
//  On success prints:  PASS bytes / PASS waits / PASS interrupts and finally
//  Z80_FETCH_TRACE_OK.
//------------------------------------------------------------------------------
#define CHIPS_IMPL
#include "chips/z80.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_TICKS   2048
#define MAX_EVENTS  1024
#define ACK_SRC_ID  ((uint64_t)77)

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

// --- host-owned event log: the callback copies the event, proving the core's
// event storage need only stay valid for the duration of the callback -------
typedef struct {
    size_t n;
    z80_trace_event_t ev[MAX_EVENTS];
} log_t;

static void log_callback(const z80_trace_event_t* ev, void* user_data) {
    log_t* log = (log_t*)user_data;
    if (log->n < MAX_EVENTS) {
        log->ev[log->n++] = *ev;
    }
}

// --- controlled, bank-aware bus ---------------------------------------------
//
// Wait model: a read-sampling decoder step keeps its step and address constant
// across its whole machine cycle, even though the core deasserts the read
// control pins on WAIT ticks. We arm on the asserted entry tick, hold WAIT for
// waits_per_read ticks, and release on the following tick, which is exactly
// when the core samples. A real host keeps the byte/source driven through the
// wait states, so the held source is returned every tick of the cycle; the
// tracer must still never attribute a BYTE to a WAIT tick.
typedef struct {
    uint8_t  mem[1u << 16];
    uint16_t bank_boundary;   // addresses < boundary use lo_src, else hi_src
    uint64_t lo_src;
    uint64_t hi_src;
    int      waits_per_read;  // WAIT ticks inserted per read machine cycle
    uint8_t  ack_byte;
    // per-tick observations
    bool     tick_was_wait[MAX_TICKS];
    uint64_t pins_trace[MAX_TICKS];
    // wait FSM
    bool     armed;
    uint16_t arm_step;
    uint16_t arm_addr;
    bool     arm_ack;
    int      wait_left;
    bool     waiting;
} bus_t;

static uint64_t bus_source(const bus_t* b, uint16_t addr) {
    return (addr < b->bank_boundary) ? b->lo_src : b->hi_src;
}

static bool ctrl_is_read(uint64_t ctrl) {
    return ((ctrl & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) ||
           ((ctrl & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ));
}

// Prepare one tick: decide WAIT, drive held read data and return the source of
// the byte the core may sample this tick. Modifies *pins so WAIT reaches core.
static uint64_t bus_pre_tick(bus_t* b, uint64_t* pins, uint16_t step, int tick) {
    const uint64_t ctrl = *pins & Z80_CTRL_PIN_MASK;
    const bool read_asserted = ctrl_is_read(ctrl);
    const bool ack_asserted = (*pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ);

    // arm on a fresh read machine cycle
    if (read_asserted && (!b->armed || step != b->arm_step)) {
        b->armed = true;
        b->arm_step = step;
        b->arm_addr = (uint16_t)Z80_GET_ADDR(*pins);
        b->arm_ack = ack_asserted;
        b->wait_left = b->waits_per_read;
        b->waiting = true;
    }
    // disarm once the core advances past the sampling step
    if (b->armed && step != b->arm_step) {
        b->armed = false;
        b->waiting = false;
    }

    bool wait = false;
    if (b->armed && b->waiting) {
        if (b->wait_left > 0) { wait = true; b->wait_left--; }
        else { b->waiting = false; }   // release: this tick is the sample
    }
    if (wait) { *pins |= Z80_WAIT; } else { *pins &= ~Z80_WAIT; }
    b->tick_was_wait[tick] = wait;

    // keep data/source held through the whole cycle like a real host
    if (b->armed && !b->arm_ack) {
        Z80_SET_DATA(*pins, b->mem[b->arm_addr]);
        return bus_source(b, b->arm_addr);
    }
    if (b->armed && b->arm_ack) {
        Z80_SET_DATA(*pins, b->ack_byte);
        return ACK_SRC_ID;
    }
    return 0;
}

static void bus_post_tick(bus_t* b, uint64_t* pins, int tick) {
    if ((*pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
        b->mem[Z80_GET_ADDR(*pins)] = Z80_GET_DATA(*pins);
    }
    // snapshot AFTER the host has driven data, so it is directly comparable to
    // the shadow run's post-drive pin mask
    b->pins_trace[tick] = *pins;
}

static void bus_init(bus_t* b) {
    memset(b, 0, sizeof(*b));
    b->bank_boundary = 0xFFFF;
    b->lo_src = 1;
    b->hi_src = 2;
    b->armed = false;
    b->arm_step = 0xFFFF;
}

static void load(uint8_t* mem, uint16_t addr, const uint8_t* bytes, size_t n) {
    memcpy(&mem[addr], bytes, n);
}

// --- runners: one traced (fills a log), one plain shadow (pins only) --------
static int run_traced(bus_t* b, z80_t* cpu, z80_trace_t* tr, log_t* log, uint16_t start,
                      int (*raise)(int tick, uint64_t* pins), int max_ticks) {
    uint64_t pins = z80_init(cpu);
    pins = z80_prefetch(cpu, start);
    memset(log, 0, sizeof(*log));
    z80_trace_init(tr, log_callback, log);
    int tick;
    for (tick = 0; tick < max_ticks; tick++) {
        if (raise && raise(tick, &pins)) { /* interrupt requested */ }
        const uint64_t src = bus_pre_tick(b, &pins, cpu->step, tick);
        pins = z80_tick_trace(cpu, pins, tr, src, (uint32_t)Z80_GET_ADDR(pins));
        bus_post_tick(b, &pins, tick);
        if ((pins & Z80_HALT) && (tick > max_ticks - 8)) break;
    }
    return tick;
}

static int run_shadow(bus_t* b, z80_t* cpu, uint16_t start,
                      int (*raise)(int tick, uint64_t* pins), int max_ticks,
                      uint64_t* out_pins) {
    uint64_t pins = z80_init(cpu);
    pins = z80_prefetch(cpu, start);
    int tick;
    for (tick = 0; tick < max_ticks; tick++) {
        if (raise && raise(tick, &pins)) { /* interrupt requested */ }
        (void)bus_pre_tick(b, &pins, cpu->step, tick);
        pins = z80_tick(cpu, pins);
        bus_post_tick(b, &pins, tick);
        if (out_pins) out_pins[tick] = pins;
        if ((pins & Z80_HALT) && (tick > max_ticks - 8)) break;
    }
    return tick;
}

// --- small log queries ------------------------------------------------------
static size_t count_type(const log_t* l, z80_trace_event_type_t t) {
    size_t n = 0;
    for (size_t i = 0; i < l->n; i++) if (l->ev[i].type == t) n++;
    return n;
}

static size_t nth_byte_index(const log_t* l, size_t k) {
    size_t seen = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (l->ev[i].type == Z80_TRACE_BYTE) {
            if (seen == k) return i;
            seen++;
        }
    }
    return (size_t)-1;
}

typedef struct {
    uint8_t value;
    z80_trace_byte_role_t role;
    uint16_t addr;
    bool has_addr;
    bool ack;
} exp_byte_t;

static void check_byte(const log_t* l, size_t k, const exp_byte_t* e, const bus_t* b) {
    const size_t idx = nth_byte_index(l, k);
    CHECK(idx != (size_t)-1, "missing byte #%zu", k);
    if (idx == (size_t)-1) return;
    const z80_trace_event_t* ev = &l->ev[idx];
    CHECK(ev->type == Z80_TRACE_BYTE, "event %zu is not a BYTE", idx);
    CHECK(ev->byte.value == e->value, "byte#%zu value %02x != %02x", k, ev->byte.value, e->value);
    CHECK(ev->byte.role == e->role, "byte#%zu role mismatch", k);
    CHECK(ev->byte.has_addr == e->has_addr, "byte#%zu has_addr mismatch", k);
    if (e->has_addr) {
        CHECK(ev->byte.addr == e->addr, "byte#%zu addr %04x != %04x", k, ev->byte.addr, e->addr);
        CHECK(ev->byte.source_id == bus_source(b, e->addr),
              "byte#%zu source %llu != bank source", k,
              (unsigned long long)ev->byte.source_id);
        CHECK(!b->tick_was_wait[ev->tick], "byte#%zu recorded on a WAIT tick %llu",
              k, (unsigned long long)ev->tick);
    }
    if (e->ack) {
        CHECK(ev->byte.source_id == Z80_TRACE_SOURCE_ACK, "ack byte not marked ACK");
        CHECK(ev->byte.source_offset == (uint32_t)ACK_SRC_ID, "ack offset not preserved");
    }
}

// every BYTE must belong to an existing, monotonic instruction id with a BEGIN
static void check_integrity(const log_t* l) {
    uint64_t max_id = 0;
    bool began[256] = {0};
    bool ended[256] = {0};
    for (size_t i = 0; i < l->n; i++) {
        const z80_trace_event_t* ev = &l->ev[i];
        if (ev->instr_id != 0) {
            CHECK(ev->instr_id < 256, "instr id out of test range");
            max_id = ev->instr_id > max_id ? ev->instr_id : max_id;
        }
        if (ev->type == Z80_TRACE_BEGIN) began[ev->instr_id] = true;
        if (ev->type == Z80_TRACE_END) ended[ev->instr_id] = true;
    }
    for (uint64_t id = 1; id <= max_id; id++) {
        CHECK(began[id], "instr %llu missing BEGIN", (unsigned long long)id);
        CHECK(ended[id], "instr %llu missing END", (unsigned long long)id);
    }
    // BEGIN ids are strictly monotonic
    uint64_t prev = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (l->ev[i].type == Z80_TRACE_BEGIN) {
            CHECK(l->ev[i].instr_id == prev + 1, "non-monotonic BEGIN id");
            prev = l->ev[i].instr_id;
        }
    }
}

// pin-for-pin equivalence of traced run vs a plain z80_tick run
static void check_pin_equivalence(const bus_t* b, int ticks, const uint64_t* shadow) {
    for (int t = 0; t < ticks; t++) {
        CHECK(b->pins_trace[t] == shadow[t],
              "tick %d pins differ: trace %016llx shadow %016llx", t,
              (unsigned long long)b->pins_trace[t],
              (unsigned long long)shadow[t]);
    }
}

//=== BYTES group =============================================================
static int raise_none(int tick, uint64_t* pins) { (void)tick; (void)pins; return 0; }

static void test_bytes(void) {
    bus_t b; bus_init(&b);
    // 0x0000 LD A,07            (3E 07)         operand at 0x0001
    // 0x0002 LD A,(0x0001)     (3A 01 00)      data read of same addr 0x0001
    // 0x0005 LD IX,0x2000      (DD 21 00 20)
    // 0x0009 BIT 0,(IX+1)      (DD CB 01 46)   data read 0x2001, non-M1 opcode
    // 0x000D NOP               (00)
    // 0x000E HALT              (76)
    const uint8_t code[] = {
        0x3E,0x07,
        0x3A,0x01,0x00,
        0xDD,0x21,0x00,0x20,
        0xDD,0xCB,0x01,0x46,
        0x00,
        0x76
    };
    load(b.mem, 0x0000, code, sizeof code);
    b.mem[0x0001] = 0x07;
    b.mem[0x2001] = 0x01;

    z80_t cpu; z80_trace_t tr; log_t log;
    int ticks = run_traced(&b, &cpu, &tr, &log, 0x0000, raise_none, 400);
    CHECK(cpu.a == 0x07, "LD A,(0001) loaded %02x want 07", cpu.a);
    CHECK(cpu.ix == 0x2000, "IX=%04x want 2000", cpu.ix);

    check_integrity(&log);
    const exp_byte_t expected[] = {
        {0x3E, Z80_TRACE_OPCODE,  0x0000, true,  false}, // LD A,n
        {0x07, Z80_TRACE_OPERAND, 0x0001, true,  false},
        {0x3A, Z80_TRACE_OPCODE,  0x0002, true,  false}, // LD A,(nn)
        {0x01, Z80_TRACE_OPERAND, 0x0003, true,  false},
        {0x00, Z80_TRACE_OPERAND, 0x0004, true,  false},
        // (0x0001) data read here must NOT produce a BYTE
        {0xDD, Z80_TRACE_PREFIX,  0x0005, true,  false}, // LD IX,nn
        {0x21, Z80_TRACE_OPCODE,  0x0006, true,  false},
        {0x00, Z80_TRACE_OPERAND, 0x0007, true,  false},
        {0x20, Z80_TRACE_OPERAND, 0x0008, true,  false},
        {0xDD, Z80_TRACE_PREFIX,  0x0009, true,  false}, // DD CB
        {0xCB, Z80_TRACE_PREFIX,  0x000A, true,  false},
        {0x01, Z80_TRACE_OPERAND, 0x000B, true,  false}, // displacement
        {0x46, Z80_TRACE_OPCODE,  0x000C, true,  false}, // non-M1 opcode read
        // (0x2001) data read here must NOT produce a BYTE
        {0x00, Z80_TRACE_OPCODE,  0x000D, true,  false}, // NOP
        {0x76, Z80_TRACE_OPCODE,  0x000E, true,  false}, // HALT
    };
    CHECK(count_type(&log, Z80_TRACE_BYTE) == sizeof expected / sizeof expected[0],
          "byte count %zu != %zu", count_type(&log, Z80_TRACE_BYTE),
          sizeof expected / sizeof expected[0]);
    for (size_t k = 0; k < sizeof expected / sizeof expected[0]; k++) {
        check_byte(&log, k, &expected[k], &b);
    }
    // the shared data address 0x0001 appears exactly once as an encoding byte
    int addr1_bytes = 0;
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i].type == Z80_TRACE_BYTE && log.ev[i].byte.has_addr &&
            log.ev[i].byte.addr == 0x0001) addr1_bytes++;
    }
    CHECK(addr1_bytes == 1, "addr 0001 encoded %d times, want 1", addr1_bytes);

    // the DD CB instruction keeps all four bytes under one id with num_bytes 4
    const z80_trace_event_t* ddcbbegin = 0;
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i].type == Z80_TRACE_BEGIN && log.ev[i].begin.pc == 0x0009) {
            ddcbbegin = &log.ev[i];
        }
    }
    CHECK(ddcbbegin != 0, "missing DD CB BEGIN");
    if (ddcbbegin) {
        int n = 0;
        const z80_trace_event_t* end = 0;
        for (size_t i = 0; i < log.n; i++) {
            if (log.ev[i].instr_id == ddcbbegin->instr_id) {
                if (log.ev[i].type == Z80_TRACE_BYTE) n++;
                if (log.ev[i].type == Z80_TRACE_END) end = &log.ev[i];
            }
        }
        CHECK(n == 4, "DD CB encoded %d bytes, want 4", n);
        CHECK(end && end->end.num_bytes == 4 && end->end.pc == 0x000D,
              "DD CB END mismatch");
    }

    // HALT: exactly one HALT instruction, then silence while spinning
    size_t halt_bytes = 0;
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i].type == Z80_TRACE_BYTE && log.ev[i].byte.value == 0x76) halt_bytes++;
    }
    CHECK(halt_bytes == 1, "HALT byte recorded %zu times, want 1", halt_bytes);
    CHECK(count_type(&log, Z80_TRACE_BEGIN) == 6, "expected 6 instructions");

    // shadow run must produce identical pins tick for tick
    bus_t sb; bus_init(&sb); memcpy(sb.mem, b.mem, sizeof b.mem);
    static uint64_t shadow_pins[MAX_TICKS];
    z80_t scpu;
    int sticks = run_shadow(&sb, &scpu, 0x0000, raise_none, 400, shadow_pins);
    CHECK(ticks == sticks, "traced/shadow tick count differ");
    check_pin_equivalence(&b, ticks, shadow_pins);
}

// repeated DD/FD prefixes must all survive under one instruction id
static void test_repeated_prefixes(void) {
    bus_t b; bus_init(&b);
    const uint8_t code[] = {0xDD,0xDD,0xFD,0x21,0x00,0x20,0x76}; // LD IY,0x2000; HALT
    load(b.mem, 0, code, sizeof code);
    z80_t cpu; z80_trace_t tr; log_t log;
    run_traced(&b, &cpu, &tr, &log, 0, raise_none, 200);
    CHECK(cpu.iy == 0x2000, "IY=%04x want 2000", cpu.iy);
    const exp_byte_t exp[] = {
        {0xDD, Z80_TRACE_PREFIX, 0, true, false},
        {0xDD, Z80_TRACE_PREFIX, 1, true, false},
        {0xFD, Z80_TRACE_PREFIX, 2, true, false},
        {0x21, Z80_TRACE_OPCODE, 3, true, false},
        {0x00, Z80_TRACE_OPERAND,4, true, false},
        {0x20, Z80_TRACE_OPERAND,5, true, false},
        {0x76, Z80_TRACE_OPCODE, 6, true, false},
    };
    CHECK(count_type(&log, Z80_TRACE_BYTE) == 7, "prefix byte count");
    for (size_t k = 0; k < 7; k++) check_byte(&log, k, &exp[k], &b);
    // all six encoding bytes of the first instruction share one id
    CHECK(log.ev[0].instr_id == log.ev[nth_byte_index(&log, 5)].instr_id,
          "long prefix chain split across instructions");
}

// adjacent encoding bytes from different banks + 16-bit wrap at 0xffff
static void test_banks_and_wrap(void) {
    bus_t b; bus_init(&b);
    b.bank_boundary = 0x0001;   // addr 0 in bank 1, addr >=1 in bank 2
    b.lo_src = 10; b.hi_src = 20;
    const uint8_t code[] = {0x3E, 0x55, 0x76}; // LD A,0x55; HALT
    load(b.mem, 0, code, sizeof code);
    z80_t cpu; z80_trace_t tr; log_t log;
    run_traced(&b, &cpu, &tr, &log, 0, raise_none, 120);
    CHECK(cpu.a == 0x55, "A=%02x want 55", cpu.a);
    const z80_trace_event_t* op = &log.ev[nth_byte_index(&log, 0)];
    const z80_trace_event_t* im = &log.ev[nth_byte_index(&log, 1)];
    CHECK(op->byte.source_id == 10, "opcode bank source");
    CHECK(im->byte.source_id == 20, "operand bank source (adjacent different bank)");

    // wrap: LD BC,nn straddling 0xffff, logical addresses wrap, sources follow
    bus_t w; bus_init(&w);
    w.bank_boundary = 0x0000;    // every addr uses hi_src except none
    w.lo_src = 1; w.hi_src = 2;
    w.mem[0xFFFE] = 0x01; w.mem[0xFFFF] = 0x34; w.mem[0x0000] = 0x12; w.mem[0x0001] = 0x76;
    z80_t wc; z80_trace_t wt; log_t wl;
    run_traced(&w, &wc, &wt, &wl, 0xFFFE, raise_none, 120);
    CHECK(wc.bc == 0x1234, "BC=%04x want 1234", wc.bc);
    const exp_byte_t wexp[] = {
        {0x01, Z80_TRACE_OPCODE,  0xFFFE, true, false},
        {0x34, Z80_TRACE_OPERAND, 0xFFFF, true, false},
        {0x12, Z80_TRACE_OPERAND, 0x0000, true, false},
        {0x76, Z80_TRACE_OPCODE,  0x0001, true, false},
    };
    CHECK(count_type(&wl, Z80_TRACE_BYTE) == 4, "wrap byte count");
    for (size_t k = 0; k < 4; k++) check_byte(&wl, k, &wexp[k], &w);
}

// LDIR: every re-fetched iteration is its own instruction even at the same PC
static void test_ldir(void) {
    bus_t b; bus_init(&b);
    b.mem[0x0100] = 0xED; b.mem[0x0101] = 0xB0; b.mem[0x0102] = 0x76;
    b.mem[0x2000] = 0xAA; b.mem[0x2001] = 0xBB;
    z80_t cpu; z80_trace_t tr; log_t log; memset(&log, 0, sizeof log);
    uint64_t pins = z80_init(&cpu); pins = z80_prefetch(&cpu, 0x0100);
    z80_trace_init(&tr, log_callback, &log);
    cpu.bc = 2; cpu.hl = 0x2000; cpu.de = 0x3000;
    for (int t = 0; t < 400; t++) {
        uint64_t src = bus_pre_tick(&b, &pins, cpu.step, t);
        pins = z80_tick_trace(&cpu, pins, &tr, src, (uint32_t)Z80_GET_ADDR(pins));
        bus_post_tick(&b, &pins, t);
        if (pins & Z80_HALT) break;
    }
    CHECK(b.mem[0x3000] == 0xAA && b.mem[0x3001] == 0xBB, "LDIR copy wrong");
    // two LDIR instruction records, same begin PC, distinct monotonic ids
    int ldir_begins = 0;
    uint64_t first_id = 0, second_id = 0;
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i].type == Z80_TRACE_BEGIN && log.ev[i].begin.pc == 0x0100) {
            if (ldir_begins == 0) first_id = log.ev[i].instr_id;
            if (ldir_begins == 1) second_id = log.ev[i].instr_id;
            ldir_begins++;
        }
    }
    CHECK(ldir_begins == 2, "LDIR iterations recorded %d, want 2", ldir_begins);
    CHECK(second_id == first_id + 1, "LDIR iterations not separately numbered");
    // the HL data read of each iteration must not be an encoding byte
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i].type == Z80_TRACE_BYTE) {
            CHECK(log.ev[i].byte.addr < 0x2000 || log.ev[i].byte.addr > 0x2001,
                  "LDIR data read leaked into encoding at %04x", log.ev[i].byte.addr);
        }
    }
}

//=== WAITS group =============================================================
static void test_waits(void) {
    // same program as the bytes group, but with two wait states per read
    bus_t b; bus_init(&b);
    b.waits_per_read = 2;
    const uint8_t code[] = {
        0x3E,0x07, 0x3A,0x01,0x00,
        0xDD,0x21,0x00,0x20,
        0xDD,0xCB,0x01,0x46, 0x00, 0x76
    };
    load(b.mem, 0, code, sizeof code);
    b.mem[0x0001] = 0x07; b.mem[0x2001] = 0x01;
    z80_t cpu; z80_trace_t tr; log_t log;
    int ticks = run_traced(&b, &cpu, &tr, &log, 0, raise_none, 900);
    CHECK(cpu.a == 0x07 && cpu.ix == 0x2000, "wait run changed execution result");

    // all 15 encoding bytes survive the waits, none lands on a WAIT tick, and
    // every one is attributed to the real bank source of its held read
    size_t bytes = count_type(&log, Z80_TRACE_BYTE);
    CHECK(bytes == 15, "wait run encoded %zu bytes, want 15", bytes);
    for (size_t k = 0; k < bytes; k++) {
        const size_t idx = nth_byte_index(&log, k);
        const z80_trace_event_t* ev = &log.ev[idx];
        // the core must sample only after WAIT is released
        CHECK(!b.tick_was_wait[ev->tick], "byte#%zu emitted during WAIT", k);
        CHECK(ev->byte.has_addr, "byte#%zu lost its logical address", k);
        // the held bank source is the one attached at the true sample tick
        CHECK(ev->byte.source_id == bus_source(&b, ev->byte.addr),
              "byte#%zu source does not match its bank", k);
    }
    // waits actually happened and every sample was delayed past its request
    int wait_ticks = 0;
    for (int t = 0; t < ticks; t++) if (b.tick_was_wait[t]) wait_ticks++;
    CHECK(wait_ticks > 0, "test harness failed to insert any WAIT");

    // equivalence with a plain, equally-waiting z80_tick run
    bus_t sb; bus_init(&sb); sb.waits_per_read = 2;
    memcpy(sb.mem, b.mem, sizeof b.mem);
    static uint64_t shadow[MAX_TICKS];
    z80_t scpu;
    int sticks = run_shadow(&sb, &scpu, 0, raise_none, 900, shadow);
    CHECK(ticks == sticks, "wait traced/shadow tick count differ");
    check_pin_equivalence(&b, ticks, shadow);
    CHECK(memcmp(&cpu, &scpu, sizeof cpu) == 0, "wait traced/shadow CPU state differ");
}

//=== INTERRUPTS group ========================================================
typedef struct { int at_tick; bool nmi; bool clear_after; } irq_t;

static irq_t g_irq;
// Raise the request at the configured tick and HOLD it. The maskable INT line
// is level-triggered in this model, so it must stay asserted until the CPU
// starts its M1|IORQ acknowledge cycle (the main loop clears it there).
static int raise_irq(int tick, uint64_t* pins) {
    int raised = 0;
    if (tick >= g_irq.at_tick) {
        if (g_irq.nmi) *pins |= Z80_NMI; else *pins |= Z80_INT;
        raised = 1;
    }
    return raised;
}

static void setup_interrupt_machine(bus_t* b, int im) {
    bus_init(b);
    b->mem[0x0000] = 0xFB;   // EI
    b->mem[0x0001] = 0x76;   // HALT (wakes on interrupt)
    b->mem[0x0038] = 0x00; b->mem[0x0039] = 0x76; // IM1 ISR: NOP; HALT
    b->mem[0x0066] = 0x00; b->mem[0x0067] = 0x76; // NMI ISR: NOP; HALT
    // IM2 vector table: I=0x00, vector 0x80 -> target 0x1234
    b->mem[0x0080] = 0x34; b->mem[0x0081] = 0x12;
    b->mem[0x1234] = 0x00; b->mem[0x1235] = 0x76;
    // IM0: after HALT pc==2, ack opcode LD A,n reads operand at pc 2
    b->mem[0x0002] = 0x5A; b->mem[0x0003] = 0x76;
    (void)im;
}

static const z80_trace_event_t* find_int(const log_t* l) {
    for (size_t i = 0; i < l->n; i++) {
        if (l->ev[i].type == Z80_TRACE_INT) return &l->ev[i];
    }
    return 0;
}

static void run_one_interrupt(int im, bool nmi, uint8_t ack, z80_trace_int_kind_t kind,
                              uint8_t want_a) {
    bus_t b; setup_interrupt_machine(&b, im);
    b.ack_byte = ack;
    z80_t cpu; z80_trace_t tr; log_t log;
    memset(&log, 0, sizeof log);
    uint64_t pins = z80_init(&cpu);
    z80_trace_init(&tr, log_callback, &log);
    cpu.im = (uint8_t)im;
    cpu.i = 0x00;
    g_irq = (irq_t){ 12, nmi, true };

    bool halt_seen = false;
    size_t events_at_halt = 0;
    size_t events_pre_irq = 0;
    for (int t = 0; t < 300; t++) {
        raise_irq(t, &pins);
        uint64_t src = bus_pre_tick(&b, &pins, cpu.step, t);
        pins = z80_tick_trace(&cpu, pins, &tr, src, (uint32_t)Z80_GET_ADDR(pins));
        bus_post_tick(&b, &pins, t);
        // release the line as soon as the acknowledge cycle is observed
        if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
            pins &= ~(Z80_INT | Z80_NMI);
        }
        if ((pins & Z80_HALT) && !halt_seen) { halt_seen = true; events_at_halt = log.n; }
        if (t == 12) events_pre_irq = log.n;   // immediately before the request
        if (t > 60 && (pins & Z80_HALT)) break;
    }
    // the HALT spin between entering HALT and the interrupt added nothing
    CHECK(halt_seen, "CPU never entered HALT");
    CHECK(events_at_halt == events_pre_irq,
          "HALT spin emitted events (%zu -> %zu)", events_at_halt, events_pre_irq);

    const z80_trace_event_t* in = find_int(&log);
    CHECK(in != 0, "missing interrupt record (im=%d nmi=%d)", im, nmi);
    if (in) {
        CHECK(in->interrupt.kind == kind, "interrupt kind mismatch");
        CHECK(in->instr_id == 0, "interrupt record must not be an instruction id");
        if (kind == Z80_TRACE_INT_IM1 || kind == Z80_TRACE_INT_NMI) {
            CHECK(!in->interrupt.has_ack, "%d must not carry ack data", kind);
        } else {
            CHECK(in->interrupt.has_ack, "%d must carry ack data", kind);
            CHECK(in->interrupt.ack_data == ack, "ack data mismatch");
            CHECK(in->interrupt.source_id == ACK_SRC_ID, "ack source not preserved");
        }
    }

    // HALT produced exactly one record and never repeated while spinning
    size_t halt_bytes = 0;
    for (size_t i = 0; i < log.n; i++) {
        if (log.ev[i].type == Z80_TRACE_BYTE && log.ev[i].byte.value == 0x76) halt_bytes++;
    }
    CHECK(halt_bytes >= 1, "HALT not recorded");

    if (kind == Z80_TRACE_INT_IM0) {
        // ack byte is the instruction opcode, ACK source, no memory address;
        // its operand comes from a normal PC memory read
        bool found_ack_opcode = false;
        for (size_t i = 0; i < log.n; i++) {
            const z80_trace_event_t* ev = &log.ev[i];
            if (ev->type == Z80_TRACE_BYTE && !ev->byte.has_addr &&
                ev->byte.source_id == Z80_TRACE_SOURCE_ACK) {
                found_ack_opcode = true;
                CHECK(ev->byte.value == ack, "IM0 ack opcode value");
                CHECK(ev->byte.role == Z80_TRACE_OPCODE, "IM0 ack must be opcode");
            }
        }
        CHECK(found_ack_opcode, "IM0 ack opcode not traced");
        CHECK(cpu.a == want_a, "IM0 post-ack operand A=%02x want %02x", cpu.a, want_a);
        // operand at pc 2 is a normal memory byte
        bool found_operand = false;
        for (size_t i = 0; i < log.n; i++) {
            if (log.ev[i].type == Z80_TRACE_BYTE && log.ev[i].byte.has_addr &&
                log.ev[i].byte.addr == 0x0002 &&
                log.ev[i].byte.role == Z80_TRACE_OPERAND) found_operand = true;
        }
        CHECK(found_operand, "IM0 instruction operand not read from PC memory");
    }

    if (kind == Z80_TRACE_INT_IM2) {
        // the vector byte is only on the INT record; the two vector-table
        // reads (0x0080/0x0081) are data and must not be encoding bytes
        for (size_t i = 0; i < log.n; i++) {
            if (log.ev[i].type == Z80_TRACE_BYTE && log.ev[i].byte.has_addr) {
                CHECK(log.ev[i].byte.addr != 0x0080 && log.ev[i].byte.addr != 0x0081,
                      "IM2 vector table read leaked into encoding");
            }
        }
    }

    if (kind == Z80_TRACE_INT_NMI) {
        // NMI's discarded M1 fetch must never synthesize an instruction/BYTE.
        // The only instructions are EI@0, HALT@1 and the NMI ISR NOP@66/HALT@67:
        // exactly two HALT bytes (one at 1, one at 67), no phantom extra fetch.
        uint16_t begin_pcs[16]; size_t nb = 0;
        for (size_t i = 0; i < log.n && nb < 16; i++) {
            if (log.ev[i].type == Z80_TRACE_BEGIN) begin_pcs[nb++] = log.ev[i].begin.pc;
        }
        const uint16_t want_pcs[] = {0x0000, 0x0001, 0x0066, 0x0067};
        CHECK(nb == 4, "NMI BEGIN count %zu, want 4", nb);
        for (size_t i = 0; i < nb && i < 4; i++) {
            CHECK(begin_pcs[i] == want_pcs[i], "NMI BEGIN[%zu]=%04x want %04x",
                  i, begin_pcs[i], want_pcs[i]);
        }
        CHECK(halt_bytes == 2, "NMI run HALT bytes %zu, want 2 (no discarded-fetch phantom)",
              halt_bytes);
    }
    check_integrity(&log);
}

static void test_interrupts(void) {
    run_one_interrupt(1, true,  0x00, Z80_TRACE_INT_NMI, 0x00);
    run_one_interrupt(1, false, 0xFF, Z80_TRACE_INT_IM1, 0x00);
    run_one_interrupt(2, false, 0x80, Z80_TRACE_INT_IM2, 0x00);
    run_one_interrupt(0, false, 0x3E, Z80_TRACE_INT_IM0, 0x5A);
}

int main(void) {
    test_bytes();
    test_repeated_prefixes();
    test_banks_and_wrap();
    test_ldir();
    if (g_failures == 0) printf("PASS bytes\n");

    const int after_bytes = g_failures;
    test_waits();
    if (g_failures == after_bytes) printf("PASS waits\n");

    const int after_waits = g_failures;
    test_interrupts();
    if (g_failures == after_waits) printf("PASS interrupts\n");

    printf("checks=%d failures=%d\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("Z80_FETCH_TRACE_OK\n");
        return 0;
    }
    return 1;
}
