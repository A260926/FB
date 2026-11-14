//------------------------------------------------------------------------------
//  z80_fetch_trace_example.c
//
//  Minimal host showing how to use the optional instruction fetch-byte tracer
//  in chips/z80.h against a banked, 16-bit address space.
//
//  The key idea: the host owns the memory mapping and tells z80_tick_trace()
//  the provenance (source id + offset) of whatever byte the CPU samples on a
//  given tick. The core records each instruction's encoding bytes at the exact
//  tick they are really sampled, which stays correct even if a bank switch
//  would make a later re-read of the same address return different data.
//
//  Build: cc -std=c11 -I. examples/z80_fetch_trace_example.c
//------------------------------------------------------------------------------
#define CHIPS_IMPL
#include "chips/z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// two logical sources, e.g. a fixed ROM and a switchable bank
#define SOURCE_ROM  1u
#define SOURCE_BANK 2u
#define ACK_SOURCE  3u

typedef struct {
    uint8_t  rom[1u << 16];
    uint8_t  bank[1u << 16];
    uint16_t bank_start;      // addresses >= bank_start are served by the bank
} memory_map_t;

// host-held copy of every event; the core's event pointer is only valid inside
// the callback, so anything worth keeping must be copied here
typedef struct {
    size_t capacity;
    size_t count;
    z80_trace_event_t* events;
} recorder_t;

static const char* role_name(z80_trace_byte_role_t r) {
    switch (r) {
        case Z80_TRACE_PREFIX:  return "prefix";
        case Z80_TRACE_OPCODE:  return "opcode";
        case Z80_TRACE_OPERAND: return "operand";
    }
    return "?";
}

static void on_trace(const z80_trace_event_t* event, void* user_data) {
    recorder_t* rec = (recorder_t*)user_data;
    if (rec->count < rec->capacity) {
        rec->events[rec->count++] = *event;   // copy: do not keep the pointer
    }
}

// provenance of the byte readable at a logical address
static uint64_t map_source(const memory_map_t* m, uint16_t addr) {
    return (addr < m->bank_start) ? SOURCE_ROM : SOURCE_BANK;
}

static uint8_t map_read(const memory_map_t* m, uint16_t addr) {
    return (addr < m->bank_start) ? m->rom[addr] : m->bank[addr];
}

int main(void) {
    memory_map_t map;
    memset(&map, 0, sizeof map);
    map.bank_start = 0x8000;

    // LD A,0x42 ; LD (0x9000),A ; HALT
    const uint8_t program[] = { 0x3E, 0x42, 0x32, 0x00, 0x90, 0x76 };
    memcpy(map.rom, program, sizeof program);

    z80_t cpu;
    z80_trace_t trace;
    recorder_t recorder;
    recorder.capacity = 256;
    recorder.count = 0;
    recorder.events = (z80_trace_event_t*)calloc(recorder.capacity,
                                                 sizeof(z80_trace_event_t));
    if (!recorder.events) return 1;

    uint64_t pins = z80_init(&cpu);

    // Attach the tracer right after z80_init(). The same call belongs directly
    // after z80_reset() or z80_prefetch(): it drops any half-finished record
    // without fabricating an END and starts a clean instruction history.
    z80_trace_init(&trace, on_trace, &recorder);

    for (int tick = 0; tick < 200; tick++) {
        // describe the source of the byte the CPU may sample this tick
        uint64_t source_id = 0;
        uint32_t source_offset = 0;
        if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
            const uint16_t addr = (uint16_t)Z80_GET_ADDR(pins);
            source_id = map_source(&map, addr);
            source_offset = addr;
        } else if ((pins & (Z80_M1 | Z80_IORQ)) == (Z80_M1 | Z80_IORQ)) {
            source_id = ACK_SOURCE;        // interrupt acknowledge cycle
        }

        pins = z80_tick_trace(&cpu, pins, &trace, source_id, source_offset);

        // drive the bus after the CPU has issued its request
        if ((pins & (Z80_MREQ | Z80_RD)) == (Z80_MREQ | Z80_RD)) {
            const uint16_t addr = (uint16_t)Z80_GET_ADDR(pins);
            Z80_SET_DATA(pins, map_read(&map, addr));
        } else if ((pins & (Z80_MREQ | Z80_WR)) == (Z80_MREQ | Z80_WR)) {
            const uint16_t addr = (uint16_t)Z80_GET_ADDR(pins);
            if (addr >= map.bank_start) {
                map.bank[addr] = (uint8_t)Z80_GET_DATA(pins);
            }
        }

        if (pins & Z80_HALT) {
            break;
        }
    }

    // replay the host-owned copy at leisure
    for (size_t i = 0; i < recorder.count; i++) {
        const z80_trace_event_t* e = &recorder.events[i];
        switch (e->type) {
            case Z80_TRACE_BEGIN:
                printf("BEGIN #%llu pc=0x%04x\n",
                       (unsigned long long)e->instr_id, e->begin.pc);
                break;
            case Z80_TRACE_BYTE:
                if (e->byte.has_addr) {
                    printf("  BYTE  0x%02x addr=0x%04x %-7s source=%llu:0x%x tick=%llu\n",
                           e->byte.value, e->byte.addr, role_name(e->byte.role),
                           (unsigned long long)e->byte.source_id,
                           e->byte.source_offset, (unsigned long long)e->tick);
                } else {
                    printf("  BYTE  0x%02x (interrupt ack) %-7s source=ACK tick=%llu\n",
                           e->byte.value, role_name(e->byte.role),
                           (unsigned long long)e->tick);
                }
                break;
            case Z80_TRACE_END:
                printf("END   #%llu pc=0x%04x encoded_bytes=%u\n",
                       (unsigned long long)e->instr_id, e->end.pc, e->end.num_bytes);
                break;
            case Z80_TRACE_INT:
                printf("INT   kind=%d has_ack=%d ack=0x%02x\n",
                       e->interrupt.kind, e->interrupt.has_ack, e->interrupt.ack_data);
                break;
        }
    }

    printf("\nA=0x%02x (want 0x42), mem[0x9000]=0x%02x (want 0x42)\n",
           cpu.a, map.bank[0x9000]);

    free(recorder.events);
    return 0;
}
