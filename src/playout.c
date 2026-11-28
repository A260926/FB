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

#define CC_PLAYOUT_PADDING 0x8080

typedef struct cc_event {
    char* id;
    uint64_t display_frame;
    caption_frame_buffer_t target;
    diff608_plan_t plan;
    size_t sent; //< plan words already pulled; an event is committed once EOC1 went out
    struct cc_event* next;
} cc_event_t;

struct cc_playout {
    uint64_t position;
    caption_frame_t rx; //< receiver model: exact dual-screen state of every pulled word
    cc_event_t* head;   //< queued events, ascending display_frame
};

// A staged queue mutation: either an existing node (with target/frame possibly
// overridden) or a brand-new node, presented in proposed display order.
typedef struct {
    cc_event_t* node; //< NULL for a not-yet-linked new event
    uint64_t display_frame;
    const caption_frame_buffer_t* target;
} playout_proposed_t;

static cc_event_t* playout_find(cc_playout_t* playout, const char* id)
{
    cc_event_t* ev;

    for (ev = playout->head; ev; ev = ev->next) {
        if (0 == strcmp(ev->id, id)) {
            return ev;
        }
    }

    return 0;
}

// Commitment point: the first EOC (second-to-last word) has actually been
// pulled. Finishing the preload early and padding does not commit the event.
static int event_committed(const cc_event_t* ev)
{
    return ev->plan.size != 0 && ev->sent + 1 >= ev->plan.size;
}

static char* id_dup(const char* id)
{
    size_t len = strlen(id);
    char* copy = (char*)malloc(len + 1);

    if (copy) {
        memcpy(copy, id, len + 1);
    }

    return copy;
}

cc_playout_t* cc_playout_create(uint64_t start_frame)
{
    cc_playout_t* playout = (cc_playout_t*)calloc(1, sizeof(cc_playout_t));

    if (!playout) {
        return 0;
    }

    playout->position = start_frame;
    caption_frame_init(&playout->rx); // receiver powers up with both screens empty
    return playout;
}

static void event_free(cc_event_t* ev)
{
    if (ev) {
        free(ev->id);
        free(ev);
    }
}

void cc_playout_free(cc_playout_t* playout)
{
    cc_event_t* ev;

    if (!playout) {
        return;
    }

    while (playout->head) {
        ev = playout->head;
        playout->head = ev->next;
        event_free(ev);
    }

    free(playout);
}

// All generated words carry valid odd parity, so decode can never error here.
static void sim_feed(caption_frame_t* sim, uint16_t word)
{
    if (!eia608_is_padding(word)) {
        caption_frame_decode(sim, word, 0.0);
    }
}

// Starts a simulation from the real receiver state of already pulled words.
static void sim_open(const cc_playout_t* playout, caption_frame_t* sim)
{
    caption_frame_init(sim);
    sim->front = playout->rx.front;
    sim->back = playout->rx.back;
    sim->state = playout->rx.state;

    if (&playout->rx.front == playout->rx.write) {
        sim->write = &sim->front;
    } else if (&playout->rx.back == playout->rx.write) {
        sim->write = &sim->back;
    } else {
        sim->write = 0;
    }
}

// Rebuilds the plan for an event whose prefix is already on the wire. The
// transmitted words are kept verbatim; only the unsent suffix is re-encoded
// against the real residual hidden screen, followed by the EOC pair.
static libcaption_stauts_t inflight_plan(const cc_event_t* ev, const caption_frame_buffer_t* hidden,
    const caption_frame_buffer_t* target, diff608_plan_t* plan)
{
    if (0 == ev->sent || ev->sent > ev->plan.size) {
        return LIBCAPTION_ERROR;
    }

    plan->size = 0;
    memcpy(plan->word, ev->plan.word, ev->sent * sizeof(uint16_t));
    plan->size = ev->sent;

    if (LIBCAPTION_OK != diff608_plan_append_rows(plan, hidden, target)) {
        return LIBCAPTION_ERROR;
    }

    return diff608_append_eoc(plan);
}

// Rehearses the whole proposed queue from the current real position and
// receiver state. On success out_plans[i] holds the exact plan to install for
// proposed[i]. Nothing in the engine is mutated on either path.
static int playout_rehearse(const cc_playout_t* playout, const playout_proposed_t* proposed, size_t count,
    diff608_plan_t* out_plans)
{
    caption_frame_t sim;
    uint64_t pos = playout->position;
    size_t i;

    sim_open(playout, &sim);

    for (i = 0; i < count; ++i) {
        cc_event_t* node = proposed[i].node;
        size_t sent = node ? node->sent : 0;
        diff608_plan_t* plan = &out_plans[i];
        size_t from;

        plan->size = 0;

        if (sent > 0) {
            // Only the head event can be in flight; it must stay first.
            if (0 != i || LIBCAPTION_OK != inflight_plan(node, &sim.back, proposed[i].target, plan)) {
                return 0;
            }
        } else if (LIBCAPTION_OK != diff608_plan_build(plan, &sim.back, proposed[i].target)) {
            return 0;
        }

        if (plan->size < 4) {
            return 0;
        }

        from = sent;

        if (plan->size - 1 == from) {
            // EOC1 already transmitted: only the mandatory duplicate remains.
            if (pos != proposed[i].display_frame + 1 || UINT64_MAX == pos) {
                return 0;
            }

            sim_feed(&sim, plan->word[from]);
            ++pos;
            continue;
        }

        // Preload every word up to (but excluding) the first EOC as early as
        // possible. The display frame may not be reached before preload ends.
        while (from < plan->size - 2) {
            if (pos >= proposed[i].display_frame || UINT64_MAX == pos) {
                return 0;
            }

            sim_feed(&sim, plan->word[from]);
            ++from;
            ++pos;
        }

        // Padding holds the line until the exact scheduled frame.
        if (pos > proposed[i].display_frame || proposed[i].display_frame >= UINT64_MAX - 1) {
            return 0;
        }

        pos = proposed[i].display_frame;
        sim_feed(&sim, plan->word[plan->size - 2]); // EOC swaps on this frame
        ++pos;
        sim_feed(&sim, plan->word[plan->size - 1]); // duplicate EOC, no swap
        ++pos;
    }

    return 1;
}

static size_t queue_count(const cc_playout_t* playout)
{
    size_t count = 0;
    const cc_event_t* ev;

    for (ev = playout->head; ev; ev = ev->next) {
        ++count;
    }

    return count;
}

// Rebuilds linked-list order from a proposed array and installs rehearsed plans.
static void queue_install(cc_playout_t* playout, const playout_proposed_t* proposed, size_t count,
    const diff608_plan_t* plans)
{
    size_t i;
    cc_event_t* tail = 0;
    playout->head = 0;

    for (i = 0; i < count; ++i) {
        cc_event_t* node = proposed[i].node;
        node->next = 0;
        node->display_frame = proposed[i].display_frame;
        node->target = *proposed[i].target;
        node->plan = plans[i];

        if (tail) {
            tail->next = node;
        } else {
            playout->head = node;
        }

        tail = node;
    }
}

cc_playout_status_t cc_playout_set(cc_playout_t* playout, const char* id, uint64_t display_frame,
    const caption_frame_t* snapshot)
{
    cc_event_t* existing;
    cc_event_t* added = 0;
    playout_proposed_t* proposed = 0;
    diff608_plan_t* plans = 0;
    caption_frame_buffer_t staged_target;
    size_t count, i, slot;
    cc_playout_status_t status = CC_PLAYOUT_OK;

    if (!playout || !id || !*id || !snapshot || strlen(id) > CC_PLAYOUT_MAX_ID ||
        !diff608_screen_validate(&snapshot->front)) {
        return CC_PLAYOUT_ERROR_ARG;
    }

    if (display_frame < playout->position) {
        return CC_PLAYOUT_ERROR_DEADLINE;
    }

    existing = playout_find(playout, id);

    if (existing && event_committed(existing)) {
        return CC_PLAYOUT_ERROR_STATE;
    }

    // An event already being transmitted must remain the next display: its
    // prefix words cannot move behind another event.
    if (existing && existing->sent > 0) {
        cc_event_t* ev;

        for (ev = playout->head; ev; ev = ev->next) {
            if (ev != existing && ev->display_frame <= display_frame) {
                return CC_PLAYOUT_ERROR_DEADLINE;
            }
        }
    }

    count = queue_count(playout) + (existing ? 0 : 1);
    proposed = (playout_proposed_t*)calloc(count, sizeof(playout_proposed_t));
    plans = (diff608_plan_t*)calloc(count, sizeof(diff608_plan_t));

    if (!proposed || !plans) {
        status = CC_PLAYOUT_ERROR_NOMEM;
        goto cleanup;
    }

    staged_target = snapshot->front; // deep copy staged outside the live queue
    slot = 0;

    {
        cc_event_t* ev;

        for (ev = playout->head; ev; ev = ev->next) {
            proposed[slot].node = ev;

            if (ev == existing) {
                proposed[slot].display_frame = display_frame;
                proposed[slot].target = &staged_target;
            } else {
                if (ev->display_frame == display_frame) {
                    status = CC_PLAYOUT_ERROR_DEADLINE;
                    goto cleanup;
                }

                proposed[slot].display_frame = ev->display_frame;
                proposed[slot].target = &ev->target;
            }

            ++slot;
        }
    }

    if (!existing) {
        added = (cc_event_t*)calloc(1, sizeof(cc_event_t));

        if (!added) {
            status = CC_PLAYOUT_ERROR_NOMEM;
            goto cleanup;
        }

        added->id = id_dup(id);

        if (!added->id) {
            event_free(added);
            added = 0;
            status = CC_PLAYOUT_ERROR_NOMEM;
            goto cleanup;
        }

        added->display_frame = display_frame;
        added->target = staged_target;
        proposed[slot].node = added;
        proposed[slot].display_frame = display_frame;
        proposed[slot].target = &added->target;
    }

    // Stable insertion sort by display frame.
    for (i = 1; i < count; ++i) {
        playout_proposed_t tmp = proposed[i];
        size_t j = i;

        while (j > 0 && proposed[j - 1].display_frame > tmp.display_frame) {
            proposed[j] = proposed[j - 1];
            --j;
        }

        proposed[j] = tmp;
    }

    for (i = 1; i < count; ++i) {
        if (proposed[i - 1].display_frame == proposed[i].display_frame) {
            status = CC_PLAYOUT_ERROR_DEADLINE;
            goto cleanup;
        }
    }

    if (!playout_rehearse(playout, proposed, count, plans)) {
        status = CC_PLAYOUT_ERROR_DEADLINE;
        goto cleanup;
    }

    queue_install(playout, proposed, count, plans);

cleanup:
    if (added && CC_PLAYOUT_OK != status) {
        event_free(added);
    }

    free(proposed);
    free(plans);
    return status;
}

cc_playout_status_t cc_playout_cancel(cc_playout_t* playout, const char* id)
{
    cc_event_t* target = 0;
    cc_event_t* ev;
    playout_proposed_t* proposed = 0;
    diff608_plan_t* plans = 0;
    size_t count, slot = 0;
    cc_playout_status_t status = CC_PLAYOUT_OK;

    if (!playout || !id || !*id) {
        return CC_PLAYOUT_ERROR_ARG;
    }

    for (ev = playout->head; ev; ev = ev->next) {
        if (0 == strcmp(ev->id, id)) {
            target = ev;
            break;
        }
    }

    if (!target) {
        return CC_PLAYOUT_ERROR_STATE;
    }

    if (event_committed(target)) {
        return CC_PLAYOUT_ERROR_STATE;
    }

    count = queue_count(playout) - 1;
    proposed = (playout_proposed_t*)calloc(count ? count : 1, sizeof(playout_proposed_t));
    plans = (diff608_plan_t*)calloc(count ? count : 1, sizeof(diff608_plan_t));

    if (!proposed || !plans) {
        status = CC_PLAYOUT_ERROR_NOMEM;
        goto cleanup;
    }

    for (ev = playout->head; ev; ev = ev->next) {
        if (ev == target) {
            continue;
        }

        proposed[slot].node = ev;
        proposed[slot].display_frame = ev->display_frame;
        proposed[slot].target = &ev->target;
        ++slot;
    }

    // Later events must still meet their deadlines from the real residual
    // receiver state; on failure the queue stays exactly as it was.
    if (!playout_rehearse(playout, proposed, count, plans)) {
        status = CC_PLAYOUT_ERROR_DEADLINE;
        goto cleanup;
    }

    queue_install(playout, proposed, count, plans);
    event_free(target);

cleanup:
    free(proposed);
    free(plans);
    return status;
}

size_t cc_playout_pull(cc_playout_t* playout, size_t count, cc_playout_record_t* out)
{
    size_t i;

    if (!playout || (count && !out)) {
        return 0;
    }

    for (i = 0; i < count; ++i) {
        cc_event_t* ev = playout->head;
        uint16_t word = CC_PLAYOUT_PADDING;

        if (ev) {
            size_t eoc1 = ev->plan.size - 2;

            if (ev->sent < eoc1) {
                // Preload the next planned word; feasibility proved the
                // scheduled EOC frame is still reachable.
                word = ev->plan.word[ev->sent];
                ++ev->sent;
                sim_feed(&playout->rx, word);
            } else if (ev->sent == eoc1) {
                // Hold with padding until the exact display frame.
                if (playout->position == ev->display_frame) {
                    word = ev->plan.word[ev->sent];
                    ++ev->sent;
                    sim_feed(&playout->rx, word); // first EOC: commit + swap
                }
            } else {
                // The duplicate EOC follows immediately on the next frame.
                word = ev->plan.word[ev->sent];
                ++ev->sent;
                sim_feed(&playout->rx, word);
            }

            if (ev->sent == ev->plan.size) {
                playout->head = ev->next;
                event_free(ev); // next event may start preloading afterwards
            }
        }

        out[i].frame = playout->position;
        out[i].cc_data = word;
        ++playout->position;
    }

    return count;
}

uint64_t cc_playout_position(const cc_playout_t* playout)
{
    return playout ? playout->position : 0;
}
