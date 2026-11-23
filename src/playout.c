/**********************************************************************************************/
/* The MIT License                                                                            */
/*                                                                                            */
/* Copyright 2016-2017 Twitch Interactive, Inc. or its affiliates. All Rights Reserved.       */
/*                                                                                            */
/* Permission is hereby granted, free of charge, to any person obtaining a copy               */
/* of this software and associated documentation files (the "Software"), to deal              */
/* in the Software without restriction, including without limitation the rights               */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell                  */
/* copies of the Software, and to permit persons to whom the Software is                      */
/* furnished to do so, subject to the following conditions:                                   */
/*                                                                                            */
/* The above copyright notice and this permission notice shall be included in                 */
/* all copies or substantial portions of the Software.                                        */
/*                                                                                            */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR                 */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,                   */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE                */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER                     */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,              */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN                  */
/* THE SOFTWARE.                                                                              */
/**********************************************************************************************/
#include "playout.h"
#include "diff608.h"
#include "eia608.h"
#include <stdlib.h>
#include <string.h>

#define PL_CHANNEL 0
#define PL_PADDING 0x8080u

typedef struct {
    char* id;
    uint64_t display; //< absolute frame of the first EOC
    caption_frame_buffer_t screen; //< desired screen (deep-copied front snapshot)
    // Preload content in transmission order. A fresh event is RCL twice plus
    // the row diff; an in-flight replacement is rebuilt suffix-only from the
    // residual hidden memory (RCL only if the receiver is not already loading),
    // since its transmitted prefix is already on the wire and the receiver may
    // never be reset. The two EOC words are timed separately, never stored here.
    uint16_t plan[CC_PLAYOUT_MAX_PLAN_WORDS + 1];
    int plan_len;
    int sent; //< preload words already transmitted
    int eoc1_sent; //< first EOC transmitted: the event is now committed
    int finished; //< both EOCs transmitted; retained only to reject edits
    int replan; //< in-flight edit needs a suffix-only rebuild
} playout_event_t;

struct cc_playout {
    uint64_t position; //< frame the next pulled record will carry
    // Model of the real receiver, advanced only by words actually transmitted.
    // Feasibility replays copy this struct, so they always start from wire truth.
    caption_frame_t recv;
    playout_event_t* events;
    size_t count;
    size_t cap;
};

static uint16_t ctrl_word(eia608_control_t cmd)
{
    return eia608_control_command(cmd, PL_CHANNEL);
}

static int add_u64(uint64_t a, uint64_t b, uint64_t* out)
{
    if (a > UINT64_MAX - b) {
        return 0;
    }

    *out = a + b;
    return 1;
}

cc_playout_t* cc_playout_create(uint64_t start_frame)
{
    cc_playout_t* p = (cc_playout_t*)calloc(1, sizeof(cc_playout_t));

    if (!p) {
        return 0;
    }

    caption_frame_init(&p->recv); // both memories start empty
    p->position = start_frame;
    return p;
}

void cc_playout_free(cc_playout_t* playout)
{
    if (!playout) {
        return;
    }

    for (size_t i = 0; i < playout->count; ++i) {
        free(playout->events[i].id);
    }

    free(playout->events);
    free(playout);
}

uint64_t cc_playout_position(const cc_playout_t* playout)
{
    return playout ? playout->position : 0;
}

static int find_event(const cc_playout_t* p, const char* id)
{
    for (size_t i = 0; i < p->count; ++i) {
        if (!strcmp(p->events[i].id, id)) {
            return (int)i;
        }
    }

    return -1;
}

// Index of the earliest event still owing words, or -1 when only history
// (finished events) remains.
static int head_event(const cc_playout_t* p)
{
    for (size_t i = 0; i < p->count; ++i) {
        if (!p->events[i].finished) {
            return (int)i;
        }
    }

    return -1;
}

static int event_cmp(const void* a, const void* b)
{
    const playout_event_t* ea = (const playout_event_t*)a;
    const playout_event_t* eb = (const playout_event_t*)b;

    if (ea->display < eb->display) {
        return -1;
    }

    if (ea->display > eb->display) {
        return 1;
    }

    return strcmp(ea->id, eb->id);
}

// Builds a fresh event's preload: RCL twice, then the row diff from
// hidden_start to the target.
static int build_full_plan(playout_event_t* ev, const caption_frame_buffer_t* hidden_start)
{
    uint16_t words[CC_PLAYOUT_MAX_PLAN_WORDS + 1];
    int n = 0;
    uint16_t rcl = ctrl_word(eia608_control_resume_caption_loading);
    int status;

    words[n++] = rcl;
    words[n++] = rcl;
    status = diff608_encode_rows(&words[n], (int)(sizeof(words) / sizeof(words[0])) - n, hidden_start, &ev->screen);

    if (status < 0) {
        return CC_PLAYOUT_INVALID;
    }

    n += status;
    memcpy(ev->plan, words, sizeof(uint16_t) * (size_t)n);
    ev->plan_len = n;
    ev->sent = 0;
    ev->eoc1_sent = 0;
    ev->replan = 0;
    return CC_PLAYOUT_OK;
}

// Rebuilds the unsent preload of an in-flight event from the residual hidden
// memory (which already reflects every word transmitted so far). RCL is only
// re-emitted when the receiver is not writing the hidden memory.
static int build_suffix_plan(playout_event_t* ev, const caption_frame_t* sim)
{
    uint16_t words[CC_PLAYOUT_MAX_PLAN_WORDS + 1];
    int n = 0;

    if (sim->write != &sim->back) {
        uint16_t rcl = ctrl_word(eia608_control_resume_caption_loading);
        words[n++] = rcl;
        words[n++] = rcl;
    }

    int status = diff608_encode_rows(&words[n], (int)(sizeof(words) / sizeof(words[0])) - n, &sim->back, &ev->screen);

    if (status < 0) {
        return CC_PLAYOUT_INVALID;
    }

    n += status;
    memcpy(ev->plan, words, sizeof(uint16_t) * (size_t)n);
    ev->plan_len = n;
    ev->sent = 0;
    ev->eoc1_sent = 0;
    ev->replan = 1;
    return CC_PLAYOUT_OK;
}

// Feeds one word to the modeled receiver; padding (a decoder no-op) is skipped
// by callers, and timestamps only feed status bookkeeping, never the memories.
static int model_word(caption_frame_t* sim, uint16_t word)
{
    return LIBCAPTION_ERROR == caption_frame_decode(sim, word, sim->timestamp + 1.0 / 30.0) ? CC_PLAYOUT_INVALID : CC_PLAYOUT_OK;
}

static int screen_matches_front(const caption_frame_t* sim, const caption_frame_buffer_t* target)
{
    for (int r = 0; r < SCREEN_ROWS; ++r) {
        for (int c = 0; c < SCREEN_COLS; ++c) {
            if (sim->front.cell[r][c].data[0] != target->cell[r][c].data[0]) {
                return 0;
            }
        }
    }

    return 1;
}

// Replays the whole candidate queue against a copy of the real wire state and,
// on success, records each event's plan. Every event must keep its exact
// display frame; otherwise the call fails before any candidate is adopted.
// Timing is proved arithmetically (padding never touches the receiver) while
// every word actually transmitted is checked against a real decoder copy.
static int replay_queue(cc_playout_t* p, playout_event_t* events, size_t count)
{
    caption_frame_t sim = p->recv;

    // The struct copy duplicates the internal write pointer; re-anchor it to
    // the copy's own buffers so modeled writes never touch the live receiver.
    if (p->recv.write == &p->recv.back) {
        sim.write = &sim.back;
    } else if (p->recv.write == &p->recv.front) {
        sim.write = &sim.front;
    } else {
        sim.write = 0;
    }

    uint64_t pos = p->position;

    for (size_t i = 0; i < count; ++i) {
        playout_event_t* ev = &events[i];
        uint64_t end_frame;

        if (ev->finished) {
            continue; // history: both EOCs already on the wire, no budget left
        }

        if (!add_u64(ev->display, 2, &end_frame)) {
            return CC_PLAYOUT_OVERFLOW;
        }

        if (ev->eoc1_sent) {
            // Committed: only the duplicate EOC remains, due at display + 1.
            if (pos > ev->display + 1) {
                return CC_PLAYOUT_LATE;
            }

            // The decoder suppresses this duplicate, so neither memory moves.
            if (CC_PLAYOUT_OK != model_word(&sim, ctrl_word(eia608_control_end_of_caption))) {
                return CC_PLAYOUT_INVALID;
            }
        } else {
            if (ev->replan) {
                int status = build_suffix_plan(ev, &sim);

                if (CC_PLAYOUT_OK != status) {
                    return status;
                }
            } else if (0 == ev->sent) {
                int status = build_full_plan(ev, &sim.back);

                if (CC_PLAYOUT_OK != status) {
                    return status;
                }
            }
            // An unchanged in-flight event keeps its plan; sim already holds
            // the effect of its transmitted prefix and only the suffix remains.

            int remaining = ev->plan_len - ev->sent;
            uint64_t needed;

            if (remaining < 0 || pos > ev->display || !add_u64(pos, (uint64_t)remaining, &needed) || needed > ev->display) {
                // Preload must finish before the first EOC frame; fail rather
                // than silently moving the display frame.
                return CC_PLAYOUT_LATE;
            }

            // Decode every preload word still owed, starting as early as
            // possible. Padding fills any gap before the timed EOC.
            for (int w = ev->sent; w < ev->plan_len; ++w) {
                int status = model_word(&sim, ev->plan[w]);

                if (CC_PLAYOUT_OK != status) {
                    return status;
                }
            }

            // Timed EOC pair: first swaps the memories exactly on the display
            // frame, the duplicate follows immediately and is suppressed by the
            // decoder's duplicate-command handling.
            if (CC_PLAYOUT_OK != model_word(&sim, ctrl_word(eia608_control_end_of_caption))) {
                return CC_PLAYOUT_INVALID;
            }

            if (CC_PLAYOUT_OK != model_word(&sim, ctrl_word(eia608_control_end_of_caption))) {
                return CC_PLAYOUT_INVALID;
            }
        }

        if (!screen_matches_front(&sim, &ev->screen)) {
            return CC_PLAYOUT_INVALID;
        }

        pos = end_frame; // next event may not preload before the second EOC
    }

    return CC_PLAYOUT_OK;
}

static int grow_events(playout_event_t** events, size_t* cap, size_t need)
{
    size_t next = *cap ? *cap : 4;

    while (next < need) {
        if (next > SIZE_MAX / 2) {
            return CC_PLAYOUT_OVERFLOW;
        }

        next *= 2;
    }

    playout_event_t* grown = (playout_event_t*)realloc(*events, next * sizeof(playout_event_t));

    if (!grown) {
        return CC_PLAYOUT_OVERFLOW;
    }

    *events = grown;
    *cap = next;
    return CC_PLAYOUT_OK;
}

static void discard_queue(playout_event_t* events, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        free(events[i].id);
    }

    free(events);
}

// Copies an event into the candidate queue; the id string is re-owned.
static int copy_event(playout_event_t* dst, const playout_event_t* src)
{
    char* id = (char*)malloc(strlen(src->id) + 1);

    if (!id) {
        return CC_PLAYOUT_OVERFLOW;
    }

    dst->id = id;
    strcpy(dst->id, src->id);
    dst->display = src->display;
    dst->screen = src->screen;
    dst->plan_len = src->plan_len;
    dst->sent = src->sent;
    dst->eoc1_sent = src->eoc1_sent;
    dst->finished = src->finished;
    dst->replan = src->replan;
    memcpy(dst->plan, src->plan, sizeof(dst->plan));
    return CC_PLAYOUT_OK;
}

int cc_playout_set(cc_playout_t* playout, const char* id, uint64_t display_frame, const caption_frame_t* screen)
{
    if (!playout || !id || !*id || strlen(id) >= CC_PLAYOUT_MAX_ID || !screen) {
        return CC_PLAYOUT_INVALID;
    }

    if (!diff608_screen_valid(screen)) {
        return CC_PLAYOUT_INVALID;
    }

    int existing = find_event(playout, id);

    if (existing >= 0 && playout->events[existing].eoc1_sent) {
        return CC_PLAYOUT_COMMITTED;
    }

    for (size_t i = 0; i < playout->count; ++i) {
        // Finished events are history and no longer own their display frame.
        if (!playout->events[i].finished && playout->events[i].display == display_frame && (existing < 0 || (size_t)existing != i)) {
            return CC_PLAYOUT_CONFLICT;
        }
    }

    playout_event_t* candidate = 0;
    size_t candidate_count = 0, candidate_cap = 0;
    int status = grow_events(&candidate, &candidate_cap, playout->count + 1);

    if (CC_PLAYOUT_OK != status) {
        return status;
    }

    for (size_t i = 0; i < playout->count; ++i) {
        if (existing >= 0 && i == (size_t)existing) {
            continue; // replaced below
        }

        status = copy_event(&candidate[candidate_count], &playout->events[i]);

        if (CC_PLAYOUT_OK != status) {
            discard_queue(candidate, candidate_count);
            return status;
        }

        ++candidate_count;
    }

    char* new_id = (char*)malloc(strlen(id) + 1);

    if (!new_id) {
        discard_queue(candidate, candidate_count);
        return CC_PLAYOUT_OVERFLOW;
    }

    strcpy(new_id, id);
    candidate[candidate_count].id = new_id;
    candidate[candidate_count].display = display_frame;
    candidate[candidate_count].screen = screen->front; // deep copy of the snapshot

    if (existing >= 0) {
        // Preserve the transmitted prefix; an in-flight replacement is rebuilt
        // suffix-only from the residual hidden memory.
        candidate[candidate_count].plan_len = playout->events[existing].plan_len;
        candidate[candidate_count].sent = playout->events[existing].sent;
        candidate[candidate_count].eoc1_sent = 0;
        candidate[candidate_count].finished = 0;
        candidate[candidate_count].replan = playout->events[existing].sent > 0;
        memcpy(candidate[candidate_count].plan, playout->events[existing].plan, sizeof(candidate[candidate_count].plan));
    } else {
        candidate[candidate_count].plan_len = 0;
        candidate[candidate_count].sent = 0;
        candidate[candidate_count].eoc1_sent = 0;
        candidate[candidate_count].finished = 0;
        candidate[candidate_count].replan = 0;
    }

    ++candidate_count;
    qsort(candidate, candidate_count, sizeof(playout_event_t), event_cmp);

    // Replay against the current real state; the live queue stays untouched
    // until the whole candidate schedule, later events included, proves feasible.
    status = replay_queue(playout, candidate, candidate_count);

    if (CC_PLAYOUT_OK != status) {
        discard_queue(candidate, candidate_count);
        return status;
    }

    for (size_t i = 0; i < playout->count; ++i) {
        free(playout->events[i].id);
    }

    free(playout->events);
    playout->events = candidate;
    playout->cap = candidate_cap;
    playout->count = candidate_count;
    return CC_PLAYOUT_OK;
}

int cc_playout_cancel(cc_playout_t* playout, const char* id)
{
    if (!playout || !id || !*id) {
        return CC_PLAYOUT_INVALID;
    }

    int existing = find_event(playout, id);

    if (existing < 0) {
        return CC_PLAYOUT_NOT_FOUND;
    }

    if (playout->events[existing].eoc1_sent) {
        return CC_PLAYOUT_COMMITTED;
    }

    playout_event_t* candidate = 0;
    size_t candidate_count = 0, candidate_cap = 0;
    int status = grow_events(&candidate, &candidate_cap, playout->count);

    if (CC_PLAYOUT_OK != status) {
        return status;
    }

    for (size_t i = 0; i < playout->count; ++i) {
        if (i == (size_t)existing) {
            continue; // words already sent stay on the wire; only the event goes
        }

        status = copy_event(&candidate[candidate_count], &playout->events[i]);

        if (CC_PLAYOUT_OK != status) {
            discard_queue(candidate, candidate_count);
            return status;
        }

        ++candidate_count;
    }

    qsort(candidate, candidate_count, sizeof(playout_event_t), event_cmp);
    status = replay_queue(playout, candidate, candidate_count);

    if (CC_PLAYOUT_OK != status) {
        discard_queue(candidate, candidate_count);
        return status;
    }

    for (size_t i = 0; i < playout->count; ++i) {
        free(playout->events[i].id);
    }

    free(playout->events);
    playout->events = candidate;
    playout->cap = candidate_cap;
    playout->count = candidate_count;
    return CC_PLAYOUT_OK;
}

int64_t cc_playout_pull(cc_playout_t* playout, uint64_t count, cc_playout_record_t* out)
{
    if (!playout || (count && !out)) {
        return CC_PLAYOUT_INVALID;
    }

    for (uint64_t i = 0; i < count; ++i) {
        uint16_t word = PL_PADDING;
        int head = head_event(playout);

        if (head >= 0) {
            playout_event_t* ev = &playout->events[head];

            if (ev->eoc1_sent) {
                // Mandatory duplicate EOC on the frame right after the first.
                if (playout->position >= ev->display + 1) {
                    word = ctrl_word(eia608_control_end_of_caption);
                    // Retain the event as history so its id keeps rejecting
                    // edits; it no longer owes any words.
                    ev->finished = 1;
                }
            } else if (playout->position == ev->display) {
                // First EOC takes effect exactly on the display frame. Feasibility
                // checks guaranteed the whole preload finished earlier.
                word = ctrl_word(eia608_control_end_of_caption);
                ev->eoc1_sent = 1;
            } else if (playout->position < ev->display && ev->sent < ev->plan_len) {
                // Preload as early as possible, one word per frame.
                word = ev->plan[ev->sent];
                ++ev->sent;
            }
            // Otherwise idle padding while waiting for the display frame.
        }

        if (LIBCAPTION_ERROR == caption_frame_decode(&playout->recv, word, (double)playout->position / 30.0)) {
            return CC_PLAYOUT_INVALID;
        }

        out[i].frame = playout->position;
        out[i].cc_data = word;

        if (!add_u64(playout->position, 1, &playout->position)) {
            return CC_PLAYOUT_OVERFLOW;
        }
    }

    return (int64_t)count;
}
