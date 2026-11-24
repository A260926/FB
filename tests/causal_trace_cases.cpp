// CausalTrace evidence-graph cases.
//
// Groups (driven by task_support/driver.c):
//   identity - event ids, endpoints, effective actions and clocks match the
//              existing inspect_schedule snapshots; ownership/errors.
//   edges    - provenance edges and explain() path selection for every
//              supported synchronization family.
//   cancel   - max_events prefix/cancellation semantics.
//   cli      - actually launches the built dpor explain subcommand and
//              cross-checks its output against the library.

#include "model/causal_trace.hpp"
#include "model/checker.hpp"
#include "task_support/check.h"

#include <sys/wait.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using model::Action;
using model::ActionKind;
using model::CausalTrace;
using model::ModelChecker;
using model::Program;
using model::Schedule;
using model::ScheduleStep;
using model::TimedWaitTransition;

Action action(ActionKind kind) {
    Action result;
    result.kind = kind;
    return result;
}

ScheduleStep step(model::ThreadId thread, std::uint32_t index) {
    return ScheduleStep{thread, index, std::nullopt};
}

bool has_edge(const CausalTrace& trace,
              std::size_t from,
              std::size_t to,
              const std::string& reason_fragment) {
    for (const auto& edge : trace.edges) {
        if (edge.from == from && edge.to == to &&
            edge.reason.find(reason_fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool strict_clock_hb(const std::vector<std::uint64_t>& lhs,
                     const std::vector<std::uint64_t>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    bool strict = false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] > rhs[i]) {
            return false;
        }
        if (lhs[i] < rhs[i]) {
            strict = true;
        }
    }
    return strict;
}

// Every returned path is contiguous and every edge reason is non-empty.
bool path_well_formed(const CausalTrace& trace,
                      std::size_t from,
                      std::size_t to,
                      const std::vector<std::size_t>& path) {
    std::size_t cursor = from;
    for (std::size_t edge_index : path) {
        if (edge_index >= trace.edges.size()) {
            return false;
        }
        const auto& edge = trace.edges[edge_index];
        if (edge.from != cursor || edge.reason.empty()) {
            return false;
        }
        cursor = edge.to;
    }
    return cursor == to;
}

Program barrier_program() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action write = action(ActionKind::Write);
    write.address = "x";
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    Action barrier = action(ActionKind::BarrierWait);
    barrier.barrier = "phase";
    barrier.parties = 2;
    Action yield = action(ActionKind::Yield);
    return Program{{
        {lock, write, unlock, barrier, yield, barrier},
        {lock, write, unlock, barrier, yield, barrier},
    }};
}

Schedule barrier_schedule() {
    Schedule schedule;
    for (std::uint32_t i = 0; i < 4; ++i) {
        schedule.push_back(step(0, i));
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        schedule.push_back(step(1, i));
    }
    schedule.push_back(step(0, 4));
    schedule.push_back(step(0, 5));
    schedule.push_back(step(1, 4));
    schedule.push_back(step(1, 5));
    return schedule;
}

// ---------------------------------------------------------------------------
// identity group
// ---------------------------------------------------------------------------

int identity_basic_ids() {
    Program program{{{action(ActionKind::Yield), action(ActionKind::Yield)}}};
    Schedule schedule{step(0, 0), step(0, 1)};
    ModelChecker checker(program);
    CausalTrace trace = checker.causal_trace(schedule);

    TASK_CHECK(!trace.cancelled);
    TASK_CHECK(trace.events.size() == 2);
    TASK_CHECK(trace.events[0].id == 0);
    TASK_CHECK(trace.events[1].id == 1);
    TASK_CHECK(trace.events[0].endpoint == schedule[0]);
    TASK_CHECK(trace.events[1].endpoint == schedule[1]);
    TASK_CHECK(!trace.events[0].clock_after.empty());
    TASK_CHECK(!trace.events[1].clock_after.empty());

    std::vector<std::size_t> path = trace.explain(0, 1);
    TASK_CHECK(!path.empty());
    TASK_CHECK(path_well_formed(trace, 0, 1, path));
    TASK_CHECK(!trace.edges.at(path.front()).reason.empty());

    TASK_CHECK(trace.explain(0, 0).empty());
    bool threw = false;
    try {
        (void)trace.explain(0, 2);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

int identity_matches_inspect_and_owns_data() {
    Program program = barrier_program();
    Schedule schedule = barrier_schedule();

    CausalTrace trace;
    {
        ModelChecker checker(program);
        // The trace must own everything: it survives the checker scope.
        trace = checker.causal_trace(schedule);
        auto inspected = checker.inspect_schedule(schedule);
        TASK_CHECK(inspected.size() == schedule.size());
        TASK_CHECK(trace.events.size() == inspected.size());
        for (std::size_t i = 0; i < trace.events.size(); ++i) {
            TASK_CHECK(trace.events[i].id == i);
            TASK_CHECK(trace.events[i].endpoint == inspected[i].endpoint);
            TASK_CHECK(trace.events[i].effective_action ==
                        inspected[i].effective_action);
            TASK_CHECK(trace.events[i].clock_after == inspected[i].clock_after);
        }
    }

    TASK_CHECK(trace.events.size() == 12);
    // Value copy owns independent storage and explains identically.
    CausalTrace copy = trace;
    TASK_CHECK(copy.explain(1, 5) == trace.explain(1, 5));

    bool threw = false;
    try {
        (void)trace.explain(12, 0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    TASK_CHECK(threw);
    threw = false;
    try {
        (void)trace.explain(0, 100);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    TASK_CHECK(threw);

    // Every edge points forward in commit order and carries a reason.
    for (const auto& edge : trace.edges) {
        TASK_CHECK(edge.from < edge.to);
        TASK_CHECK(!edge.reason.empty());
    }

    // On this fully supported SC trace, explain reachability agrees with
    // strict vector-clock happens-before for every event pair.
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
        for (std::size_t j = 0; j < trace.events.size(); ++j) {
            const bool clock_ordered =
                strict_clock_hb(trace.events[i].clock_after,
                                trace.events[j].clock_after);
            const bool explained = !trace.explain(i, j).empty();
            TASK_CHECK(clock_ordered == explained);
        }
    }
    return 0;
}

int identity_empty_trace_errors() {
    CausalTrace empty;
    TASK_CHECK(empty.events.empty());
    // Bounds are validated first: no id exists in an empty trace.
    bool threw = false;
    try {
        (void)empty.explain(0, 0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    TASK_CHECK(threw);
    threw = false;
    try {
        (void)empty.explain(0, 1);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    TASK_CHECK(threw);

    // A present event queried against itself is the reflexive case and is
    // empty without throwing.
    CausalTrace one;
    one.events.resize(1);
    TASK_CHECK(one.explain(0, 0).empty());
    return 0;
}

// ---------------------------------------------------------------------------
// edges group
// ---------------------------------------------------------------------------

int edges_program_order() {
    Program program{{{action(ActionKind::Yield),
                      action(ActionKind::Yield),
                      action(ActionKind::Yield)}}};
    Schedule schedule{step(0, 0), step(0, 1), step(0, 2)};
    ModelChecker checker(program);
    CausalTrace trace = checker.causal_trace(schedule);
    TASK_CHECK(trace.events.size() == 3);
    TASK_CHECK(has_edge(trace, 0, 1, "program-order"));
    TASK_CHECK(has_edge(trace, 1, 2, "program-order"));
    std::vector<std::size_t> path = trace.explain(0, 2);
    TASK_CHECK((path == std::vector<std::size_t>{0, 1}));
    TASK_CHECK(trace.explain(2, 0).empty());
    return 0;
}

int edges_mutex_handoff() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action write = action(ActionKind::Write);
    write.address = "x";
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    Program program{{{lock, write, unlock}, {lock, write, unlock}}};
    Schedule schedule{step(0, 0), step(0, 1), step(0, 2),
                      step(1, 0), step(1, 1), step(1, 2)};
    ModelChecker checker(program);
    auto replay = checker.replay(schedule);
    TASK_CHECK(!replay.first_error.has_value());
    CausalTrace trace = checker.causal_trace(schedule);

    TASK_CHECK(has_edge(trace, 2, 3, "mutex"));
    std::vector<std::size_t> path = trace.explain(1, 4);
    TASK_CHECK(!path.empty());
    TASK_CHECK(path_well_formed(trace, 1, 4, path));
    TASK_CHECK(strict_clock_hb(trace.events[1].clock_after,
                               trace.events[4].clock_after));
    // Reverse direction is concurrent/not ordered.
    TASK_CHECK(trace.explain(4, 1).empty());
    return 0;
}

int edges_spawn_join() {
    Action write_x = action(ActionKind::Write);
    write_x.address = "x";
    Action spawn = action(ActionKind::Spawn);
    spawn.target = 1;
    Action join = action(ActionKind::Join);
    join.target = 1;
    Action write_y = action(ActionKind::Write);
    write_y.address = "y";
    Program program{{{write_x, spawn, join, write_y}, {write_x}}};
    Schedule schedule{step(0, 0), step(0, 1), step(1, 0),
                      step(0, 2), step(0, 3)};
    ModelChecker checker(program);
    TASK_CHECK(!checker.replay(schedule).first_error.has_value());
    CausalTrace trace = checker.causal_trace(schedule);

    TASK_CHECK(has_edge(trace, 1, 2, "spawn"));
    TASK_CHECK(has_edge(trace, 2, 3, "join"));
    std::vector<std::size_t> path = trace.explain(2, 4);
    TASK_CHECK(!path.empty());
    TASK_CHECK(path_well_formed(trace, 2, 4, path));
    // Spawner event 0 reaches the child through the spawn edge.
    TASK_CHECK(!trace.explain(0, 2).empty());
    // After join the joiner observes the child.
    TASK_CHECK(!trace.explain(2, 3).empty());
    return 0;
}

int edges_barrier_generations() {
    CausalTrace trace;
    {
        ModelChecker checker(barrier_program());
        trace = checker.causal_trace(barrier_schedule());
    }
    TASK_CHECK(trace.events.size() == 12);
    TASK_CHECK(trace.events[3].barrier_generation.has_value());
    TASK_CHECK(*trace.events[3].barrier_generation == 0);
    TASK_CHECK(*trace.events[7].barrier_generation == 0);
    TASK_CHECK(*trace.events[9].barrier_generation == 1);
    TASK_CHECK(*trace.events[11].barrier_generation == 1);

    TASK_CHECK(has_edge(trace, 3, 7, "barrier"));
    TASK_CHECK(has_edge(trace, 9, 11, "barrier"));
    TASK_CHECK(!trace.explain(3, 7).empty());
    TASK_CHECK(!trace.explain(9, 11).empty());
    // Generation 0 evidence does not merge into the generation-1 edge set as
    // if same-PC events were one node: 9 and 11 are distinct from 3 and 7.
    TASK_CHECK(trace.events[3].id != trace.events[9].id);
    TASK_CHECK(trace.events[7].id != trace.events[11].id);

    // Mutex-protected write chain across threads.
    std::vector<std::size_t> path = trace.explain(1, 5);
    TASK_CHECK(!path.empty());
    TASK_CHECK(path_well_formed(trace, 1, 5, path));

    // Full clock/explain agreement on the barrier trace too.
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
        for (std::size_t j = 0; j < trace.events.size(); ++j) {
            TASK_CHECK(strict_clock_hb(trace.events[i].clock_after,
                                       trace.events[j].clock_after) ==
                       !trace.explain(i, j).empty());
        }
    }
    return 0;
}

int edges_same_pc_loop_generation() {
    // A real branch loop revisiting the same BarrierWait endpoint.
    Action label = action(ActionKind::Label);
    label.label = "again";
    Action barrier = action(ActionKind::BarrierWait);
    barrier.barrier = "phase";
    barrier.parties = 2;
    Action read = action(ActionKind::Read);
    read.address = "flag";
    read.destination = 0;
    Action branch = action(ActionKind::BranchNonzero);
    branch.source_register = 0;
    branch.label = "again";
    Action yield = action(ActionKind::Yield);
    Action write_one = action(ActionKind::Write);
    write_one.address = "flag";
    write_one.value = model::ValueOperand{
        model::ValueOperandKind::Immediate, 1, 0};
    Action write_zero = action(ActionKind::Write);
    write_zero.address = "flag";
    write_zero.value = model::ValueOperand{
        model::ValueOperandKind::Immediate, 0, 0};

    Program program{{
        {label, barrier, read, branch, yield},
        {write_one, barrier, write_zero, barrier},
    }};
    Schedule schedule{
        step(1, 0), step(0, 1), step(1, 1), step(0, 2), step(0, 3),
        step(1, 2), step(0, 1), step(1, 3), step(0, 2), step(0, 3),
        step(0, 4),
    };
    ModelChecker checker(program, 50);
    CausalTrace trace = checker.causal_trace(schedule);
    TASK_CHECK(trace.events.size() == 11);

    // The same endpoint (thread 0, action index 1) commits twice as distinct
    // events carrying successive barrier generations.
    TASK_CHECK(trace.events[1].endpoint == (ScheduleStep{0, 1, std::nullopt}));
    TASK_CHECK(trace.events[6].endpoint == (ScheduleStep{0, 1, std::nullopt}));
    TASK_CHECK(trace.events[1].id != trace.events[6].id);
    TASK_CHECK(*trace.events[1].barrier_generation == 0);
    TASK_CHECK(*trace.events[6].barrier_generation == 1);
    TASK_CHECK(!trace.explain(1, 2).empty());
    TASK_CHECK(!trace.explain(6, 7).empty());
    return 0;
}

int edges_condition_signal() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action wait = action(ActionKind::Wait);
    wait.condition = "c";
    wait.mutex = "m";
    Action write = action(ActionKind::Write);
    write.address = "x";
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    Action signal = action(ActionKind::Signal);
    signal.condition = "c";

    Program program{{{lock, wait, write, unlock}, {lock, signal, unlock}}};
    Schedule schedule{step(0, 0), step(0, 1), step(1, 0), step(1, 1),
                      step(1, 2), step(0, 1), step(0, 2), step(0, 3)};
    ModelChecker checker(program);
    TASK_CHECK(!checker.replay(schedule).first_error.has_value());
    CausalTrace trace = checker.causal_trace(schedule);

    // ids: 0 lock, 1 park, 2 signaler lock, 3 signal, 4 signaler unlock,
    //      5 reacquire, 6 write, 7 unlock
    TASK_CHECK(has_edge(trace, 3, 5, "condition-signal"));
    std::vector<std::size_t> path = trace.explain(3, 6);
    TASK_CHECK(!path.empty());
    TASK_CHECK(path_well_formed(trace, 3, 6, path));
    TASK_CHECK(strict_clock_hb(trace.events[3].clock_after,
                               trace.events[6].clock_after));
    return 0;
}

int edges_timed_wait_timeout_has_no_wake_edge() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action timed = action(ActionKind::TimedWait);
    timed.condition = "c";
    timed.mutex = "m";
    timed.destination = 0;
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    Action signal = action(ActionKind::Signal);
    signal.condition = "c";

    Program program{{{lock, timed, unlock}, {signal}}};
    // Timeout wins (events 1 park, 2 timeout, 3 reacquire); the late signal is
    // recorded after the waiter disappeared and must have no HB edge.
    Schedule schedule{step(0, 0), step(0, 1), step(0, 1),
                      step(0, 1), step(0, 2), step(1, 0)};
    ModelChecker checker(program);
    TASK_CHECK(!checker.replay(schedule).first_error.has_value());
    CausalTrace trace = checker.causal_trace(schedule);
    TASK_CHECK(trace.events.size() == 6);

    TASK_CHECK(trace.events[1].timed_wait_occurrence.has_value());
    TASK_CHECK(trace.events[1].timed_wait_occurrence->transition ==
               TimedWaitTransition::Park);
    TASK_CHECK(trace.events[2].timed_wait_occurrence.has_value());
    TASK_CHECK(trace.events[2].timed_wait_occurrence->transition ==
               TimedWaitTransition::Timeout);
    TASK_CHECK(!trace.events[3].timed_wait_occurrence.has_value());
    TASK_CHECK(trace.events[3].effective_action.kind == ActionKind::Lock);

    for (const auto& edge : trace.edges) {
        TASK_CHECK(edge.from != 5);
    }
    TASK_CHECK(trace.explain(5, 3).empty());
    TASK_CHECK(trace.explain(5, 4).empty());
    return 0;
}

int edges_timed_wait_signal_wins() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action timed = action(ActionKind::TimedWait);
    timed.condition = "c";
    timed.mutex = "m";
    timed.destination = 0;
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    Action signal = action(ActionKind::Signal);
    signal.condition = "c";

    Program program{{{lock, timed, unlock}, {signal}}};
    Schedule schedule{step(0, 0), step(0, 1), step(1, 0),
                      step(0, 1), step(0, 2)};
    ModelChecker checker(program);
    CausalTrace trace = checker.causal_trace(schedule);
    TASK_CHECK(trace.events.size() == 5);
    TASK_CHECK(trace.events[1].timed_wait_occurrence->transition ==
               TimedWaitTransition::Park);
    TASK_CHECK(!trace.events[3].timed_wait_occurrence.has_value());
    TASK_CHECK(trace.events[3].effective_action.kind == ActionKind::Lock);
    TASK_CHECK(has_edge(trace, 2, 3, "condition-signal"));
    TASK_CHECK(!trace.explain(2, 4).empty());
    return 0;
}

int edges_broadcast_wakes_all() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action wait = action(ActionKind::Wait);
    wait.condition = "c";
    wait.mutex = "m";
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    Action broadcast = action(ActionKind::Broadcast);
    broadcast.condition = "c";

    Program program{{{lock, wait, unlock},
                     {lock, wait, unlock},
                     {broadcast}}};
    Schedule schedule{
        step(0, 0), step(0, 1),
        step(1, 0), step(1, 1),
        step(2, 0),
        step(0, 1), step(0, 2),
        step(1, 1), step(1, 2),
    };
    ModelChecker checker(program);
    TASK_CHECK(!checker.replay(schedule).first_error.has_value());
    CausalTrace trace = checker.causal_trace(schedule);
    // Broadcast event id 4; reacquires are ids 5 and 7.
    TASK_CHECK(has_edge(trace, 4, 5, "condition-broadcast"));
    TASK_CHECK(has_edge(trace, 4, 7, "condition-broadcast"));
    TASK_CHECK(!trace.explain(4, 6).empty());
    TASK_CHECK(!trace.explain(4, 8).empty());
    return 0;
}

int edges_explain_shortest_and_lexicographic() {
    // Two equal-length paths 0-2-3 and 0-1-3, edges inserted out of order.
    CausalTrace tie;
    tie.events.resize(4);
    tie.edges.push_back(model::CausalEdge{0, 2, "a"}); // 0
    tie.edges.push_back(model::CausalEdge{2, 3, "b"}); // 1
    tie.edges.push_back(model::CausalEdge{0, 1, "c"}); // 2
    tie.edges.push_back(model::CausalEdge{1, 3, "d"}); // 3
    std::vector<std::size_t> path = tie.explain(0, 3);
    TASK_CHECK((path == std::vector<std::size_t>{2, 3}));

    // A direct edge wins over a two-edge path regardless of id order.
    CausalTrace shortest;
    shortest.events.resize(4);
    shortest.edges.push_back(model::CausalEdge{0, 1, "p"}); // 0
    shortest.edges.push_back(model::CausalEdge{1, 3, "q"}); // 1
    shortest.edges.push_back(model::CausalEdge{0, 3, "direct"}); // 2
    std::vector<std::size_t> short_path = shortest.explain(0, 3);
    TASK_CHECK((short_path == std::vector<std::size_t>{2}));

    // Unreachable.
    CausalTrace disconnected;
    disconnected.events.resize(3);
    disconnected.edges.push_back(model::CausalEdge{0, 1, "p"});
    TASK_CHECK(disconnected.explain(0, 2).empty());
    return 0;
}

int check_clock_explain_agreement(const CausalTrace& trace) {
    for (std::size_t i = 0; i < trace.events.size(); ++i) {
        for (std::size_t j = 0; j < trace.events.size(); ++j) {
            TASK_CHECK(strict_clock_hb(trace.events[i].clock_after,
                                       trace.events[j].clock_after) ==
                       !trace.explain(i, j).empty());
        }
    }
    return 0;
}

int edges_clock_explain_agreement_families() {
    // Mutex handoff.
    {
        Action lock = action(ActionKind::Lock);
        lock.mutex = "m";
        Action unlock = action(ActionKind::Unlock);
        unlock.mutex = "m";
        Program program{{{lock, unlock}, {lock, unlock}}};
        Schedule schedule{step(0, 0), step(0, 1), step(1, 0), step(1, 1)};
        TASK_CHECK(check_clock_explain_agreement(
            ModelChecker(program).causal_trace(schedule)) == 0);
    }
    // Spawn/join.
    {
        Action spawn = action(ActionKind::Spawn);
        spawn.target = 1;
        Action join = action(ActionKind::Join);
        join.target = 1;
        Program program{{{spawn, join}, {action(ActionKind::Yield)}}};
        Schedule schedule{step(0, 0), step(1, 0), step(0, 1)};
        TASK_CHECK(check_clock_explain_agreement(
            ModelChecker(program).causal_trace(schedule)) == 0);
    }
    // Condition variable signal.
    {
        Action lock = action(ActionKind::Lock);
        lock.mutex = "m";
        Action wait = action(ActionKind::Wait);
        wait.condition = "c";
        wait.mutex = "m";
        Action unlock = action(ActionKind::Unlock);
        unlock.mutex = "m";
        Action signal = action(ActionKind::Signal);
        signal.condition = "c";
        Program program{{{lock, wait, unlock}, {lock, signal, unlock}}};
        Schedule schedule{step(0, 0), step(0, 1), step(1, 0), step(1, 1),
                          step(1, 2), step(0, 1), step(0, 2)};
        TASK_CHECK(check_clock_explain_agreement(
            ModelChecker(program).causal_trace(schedule)) == 0);
    }
    // Timed wait timeout wins plus a late signal.
    {
        Action lock = action(ActionKind::Lock);
        lock.mutex = "m";
        Action timed = action(ActionKind::TimedWait);
        timed.condition = "c";
        timed.mutex = "m";
        Action unlock = action(ActionKind::Unlock);
        unlock.mutex = "m";
        Action signal = action(ActionKind::Signal);
        signal.condition = "c";
        Program program{{{lock, timed, unlock}, {signal}}};
        Schedule schedule{step(0, 0), step(0, 1), step(0, 1),
                          step(0, 1), step(0, 2), step(1, 0)};
        TASK_CHECK(check_clock_explain_agreement(
            ModelChecker(program).causal_trace(schedule)) == 0);
    }
    return 0;
}

int edges_reject_unsupported_and_nonsc() {
    Action rlock = action(ActionKind::RLock);
    rlock.rwlock = "rw";
    Program rw_program{{{rlock}}};
    bool threw = false;
    try {
        (void)ModelChecker(rw_program).causal_trace({step(0, 0)});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);

    Action semwait = action(ActionKind::SemWait);
    semwait.semaphore = "s";
    Program sem_program{{{semwait}}};
    threw = false;
    try {
        (void)ModelChecker(sem_program).causal_trace({step(0, 0)});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);

    Program yields{{{action(ActionKind::Yield)}}};
    Schedule schedule{step(0, 0)};
    threw = false;
    try {
        (void)ModelChecker(yields, ModelChecker::kDefaultStepBound,
                           model::MemoryModel::TSO)
                  .causal_trace(schedule);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

// ---------------------------------------------------------------------------
// cancel group
// ---------------------------------------------------------------------------

int cancel_positive_cap_prefix() {
    Schedule schedule = barrier_schedule();
    ModelChecker checker(barrier_program());
    CausalTrace full = checker.causal_trace(schedule);
    CausalTrace capped = checker.causal_trace(schedule, 5);

    TASK_CHECK(capped.cancelled);
    TASK_CHECK(capped.events.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        TASK_CHECK(capped.events[i].id == full.events[i].id);
        TASK_CHECK(capped.events[i].endpoint == full.events[i].endpoint);
        TASK_CHECK(capped.events[i].effective_action ==
                   full.events[i].effective_action);
        TASK_CHECK(capped.events[i].clock_after ==
                   full.events[i].clock_after);
    }
    // Every capped edge is exactly the corresponding full edge and all edges
    // targeting the committed prefix are retained.
    std::size_t expected_edges = 0;
    for (const auto& edge : full.edges) {
        if (edge.to < 5) {
            ++expected_edges;
        }
    }
    TASK_CHECK(capped.edges.size() == expected_edges);
    for (std::size_t i = 0; i < capped.edges.size(); ++i) {
        TASK_CHECK(capped.edges[i].from == full.edges[i].from);
        TASK_CHECK(capped.edges[i].to == full.edges[i].to);
        TASK_CHECK(capped.edges[i].reason == full.edges[i].reason);
        TASK_CHECK(capped.edges[i].to < 5);
    }
    return 0;
}

int cancel_api_consumer_shape() {
    Program program{{{action(ActionKind::Yield), action(ActionKind::Yield)}}};
    Schedule schedule{step(0, 0), step(0, 1)};
    CausalTrace prefix = ModelChecker(program).causal_trace(schedule, 1);
    TASK_CHECK(prefix.cancelled);
    TASK_CHECK(prefix.events.size() == 1);
    TASK_CHECK(prefix.events[0].id == 0);
    return 0;
}

int cancel_zero_and_overshoot() {
    Schedule schedule = barrier_schedule();
    ModelChecker checker(barrier_program());
    CausalTrace full = checker.causal_trace(schedule);
    CausalTrace zero = checker.causal_trace(schedule, 0);
    TASK_CHECK(!zero.cancelled);
    TASK_CHECK(zero.events.size() == full.events.size());
    TASK_CHECK(zero.edges.size() == full.edges.size());

    CausalTrace exact = checker.causal_trace(schedule, schedule.size());
    TASK_CHECK(!exact.cancelled);
    TASK_CHECK(exact.events.size() == schedule.size());

    CausalTrace overshoot = checker.causal_trace(schedule, schedule.size() + 40);
    TASK_CHECK(!overshoot.cancelled);
    TASK_CHECK(overshoot.events.size() == schedule.size());
    TASK_CHECK(overshoot.edges.size() == full.edges.size());
    return 0;
}

int cancel_then_full_matches_fresh() {
    Schedule schedule = barrier_schedule();
    ModelChecker checker(barrier_program());
    (void)checker.causal_trace(schedule, 4);
    CausalTrace again = checker.causal_trace(schedule);
    ModelChecker fresh_checker(barrier_program());
    CausalTrace fresh = fresh_checker.causal_trace(schedule);
    TASK_CHECK(again.cancelled == fresh.cancelled);
    TASK_CHECK(again.events.size() == fresh.events.size());
    TASK_CHECK(again.edges.size() == fresh.edges.size());
    for (std::size_t i = 0; i < again.events.size(); ++i) {
        TASK_CHECK(again.events[i].clock_after == fresh.events[i].clock_after);
    }
    for (std::size_t i = 0; i < again.edges.size(); ++i) {
        TASK_CHECK(again.edges[i].from == fresh.edges[i].from);
        TASK_CHECK(again.edges[i].to == fresh.edges[i].to);
        TASK_CHECK(again.edges[i].reason == fresh.edges[i].reason);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// cli group: actually spawn the built .task-build/dpor binary.
// ---------------------------------------------------------------------------

namespace cli_support {

namespace fs = std::filesystem;

std::string shell_quote(const fs::path& path) {
    std::string value = path.string();
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

fs::path locate_dpor(fs::path& build_dir) {
    if (const char* env_binary = std::getenv("DPOR_BIN")) {
        fs::path candidate(env_binary);
        if (fs::exists(candidate)) {
            build_dir = candidate.parent_path();
            if (build_dir.empty()) {
                build_dir = ".";
            }
            return candidate;
        }
    }
    fs::path current = fs::current_path();
    for (int depth = 0; depth < 6; ++depth) {
        fs::path candidate = current / ".task-build" / "dpor";
        if (fs::exists(candidate)) {
            build_dir = current / ".task-build";
            return candidate;
        }
        if (!current.has_parent_path() || current.parent_path() == current) {
            break;
        }
        current = current.parent_path();
    }
    build_dir = ".task-build";
    return build_dir / "dpor";
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

int run_dpor_explain(const std::string& extra_args,
                     std::string& stdout_text,
                     const fs::path& dpor,
                     const fs::path& program_file,
                     const fs::path& schedule_file,
                     const fs::path& stdout_file,
                     const fs::path& stderr_file) {
    std::ostringstream command;
    command << shell_quote(dpor) << " explain "
            << shell_quote(program_file)
            << " --schedule " << shell_quote(schedule_file)
            << " " << extra_args
            << " > " << shell_quote(stdout_file)
            << " 2> " << shell_quote(stderr_file);
    const int status = std::system(command.str().c_str());
    stdout_text = read_file(stdout_file);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

struct CliOutput {
    std::size_t events{0};
    std::size_t edges{0};
    int cancelled{-1};
    std::vector<std::size_t> path;
    std::map<std::size_t, model::CausalEdge> path_edges;
};

CliOutput parse_cli(const std::string& text) {
    CliOutput parsed;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream line_stream(line);
        std::string tag;
        line_stream >> tag;
        if (tag == "CAUSAL") {
            std::string events_token;
            std::string edges_token;
            std::string cancelled_token;
            line_stream >> events_token >> edges_token >> cancelled_token;
            auto parse_kv = [](const std::string& token, const std::string& key) {
                const std::string prefix = key + "=";
                return static_cast<std::size_t>(
                    std::stoull(token.substr(prefix.size())));
            };
            parsed.events = parse_kv(events_token, "events");
            parsed.edges = parse_kv(edges_token, "edges");
            parsed.cancelled = static_cast<int>(parse_kv(cancelled_token, "cancelled"));
        } else if (tag == "PATH") {
            std::size_t value;
            while (line_stream >> value) {
                parsed.path.push_back(value);
            }
        } else if (tag == "EDGE") {
            std::size_t index;
            std::size_t from;
            std::size_t to;
            line_stream >> index >> from >> to;
            std::string reason;
            std::getline(line_stream, reason);
            if (!reason.empty() && reason.front() == ' ') {
                reason.erase(reason.begin());
            }
            parsed.path_edges.emplace(
                index, model::CausalEdge{from, to, reason});
        }
    }
    return parsed;
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path);
    output << contents;
}

Program cli_program() {
    Action lock = action(ActionKind::Lock);
    lock.mutex = "m";
    Action write_one = action(ActionKind::Write);
    write_one.address = "x";
    write_one.value = model::ValueOperand{
        model::ValueOperandKind::Immediate, 1, 0};
    Action write_two = action(ActionKind::Write);
    write_two.address = "x";
    write_two.value = model::ValueOperand{
        model::ValueOperandKind::Immediate, 2, 0};
    Action unlock = action(ActionKind::Unlock);
    unlock.mutex = "m";
    return Program{{{lock, write_one, unlock}, {lock, write_two, unlock}}};
}

const char* kProgramText =
    "thread:\n"
    "  lock m\n"
    "  write x 1\n"
    "  unlock m\n"
    "thread:\n"
    "  lock m\n"
    "  write x 2\n"
    "  unlock m\n";

const char* kScheduleText =
    "0 0\n0 1\n0 2\n"
    "1 0\n1 1\n1 2\n";

} // namespace cli_support

int cli_explain_matches_library() {
    namespace fs = std::filesystem;
    fs::path build_dir;
    fs::path dpor = cli_support::locate_dpor(build_dir);
    TASK_CHECK(fs::exists(dpor));

    const fs::path program_file = build_dir / "causal_cli_case.dpor";
    const fs::path schedule_file = build_dir / "causal_cli_case.schedule";
    const fs::path stdout_file = build_dir / "causal_cli_case.out";
    const fs::path stderr_file = build_dir / "causal_cli_case.err";
    cli_support::write_file(program_file, cli_support::kProgramText);
    cli_support::write_file(schedule_file, cli_support::kScheduleText);

    Schedule schedule{step(0, 0), step(0, 1), step(0, 2),
                      step(1, 0), step(1, 1), step(1, 2)};
    CausalTrace library = ModelChecker(cli_support::cli_program()).causal_trace(schedule);
    std::vector<std::size_t> expected_path = library.explain(1, 4);
    TASK_CHECK(!expected_path.empty());

    std::string stdout_text;
    const int exit_code = cli_support::run_dpor_explain(
        "--from 1 --to 4", stdout_text, dpor, program_file, schedule_file,
        stdout_file, stderr_file);
    TASK_CHECK(exit_code == 0);

    cli_support::CliOutput parsed = cli_support::parse_cli(stdout_text);
    TASK_CHECK(parsed.events == library.events.size());
    TASK_CHECK(parsed.edges == library.edges.size());
    TASK_CHECK(parsed.cancelled == 0);
    TASK_CHECK(parsed.path == expected_path);
    for (std::size_t edge_index : expected_path) {
        auto found = parsed.path_edges.find(edge_index);
        TASK_CHECK(found != parsed.path_edges.end());
        const model::CausalEdge& expected_edge = library.edges[edge_index];
        TASK_CHECK(found->second.from == expected_edge.from);
        TASK_CHECK(found->second.to == expected_edge.to);
        TASK_CHECK(found->second.reason == expected_edge.reason);
        TASK_CHECK(!found->second.reason.empty());
    }

    // max-events cross-check.
    CausalTrace capped =
        ModelChecker(cli_support::cli_program()).causal_trace(schedule, 3);
    std::vector<std::size_t> capped_path = capped.explain(0, 2);
    std::string capped_out;
    const int capped_exit = cli_support::run_dpor_explain(
        "--from 0 --to 2 --max-events 3", capped_out, dpor, program_file,
        schedule_file, build_dir / "causal_cli_cap.out",
        build_dir / "causal_cli_cap.err");
    TASK_CHECK(capped_exit == 0);
    cli_support::CliOutput capped_parsed = cli_support::parse_cli(capped_out);
    TASK_CHECK(capped_parsed.events == 3);
    TASK_CHECK(capped_parsed.cancelled == 1);
    TASK_CHECK(capped_parsed.path == capped_path);
    return 0;
}

int cli_stable_for_unordered_and_reruns() {
    namespace fs = std::filesystem;
    fs::path build_dir;
    fs::path dpor = cli_support::locate_dpor(build_dir);
    TASK_CHECK(fs::exists(dpor));
    const fs::path program_file = build_dir / "causal_cli_stable.dpor";
    const fs::path schedule_file = build_dir / "causal_cli_stable.schedule";
    cli_support::write_file(program_file, cli_support::kProgramText);
    cli_support::write_file(schedule_file, cli_support::kScheduleText);

    const fs::path out1 = build_dir / "causal_cli_stable1.out";
    const fs::path err1 = build_dir / "causal_cli_stable1.err";
    const fs::path out2 = build_dir / "causal_cli_stable2.out";
    const fs::path err2 = build_dir / "causal_cli_stable2.err";
    std::string first;
    std::string second;
    int code1 = cli_support::run_dpor_explain(
        "--from 4 --to 1", first, dpor, program_file, schedule_file, out1, err1);
    int code2 = cli_support::run_dpor_explain(
        "--from 4 --to 1", second, dpor, program_file, schedule_file, out2, err2);
    TASK_CHECK(code1 == 0 && code2 == 0);

    // Reverse query is concurrent/not HB: empty path, stable across reruns.
    cli_support::CliOutput parsed1 = cli_support::parse_cli(first);
    cli_support::CliOutput parsed2 = cli_support::parse_cli(second);
    TASK_CHECK(parsed1.path.empty());
    TASK_CHECK(parsed2.path.empty());
    TASK_CHECK(first == second);

    // A positive explanation is byte-stable across reruns as well.
    std::string positive1;
    std::string positive2;
    (void)cli_support::run_dpor_explain(
        "--from 0 --to 5", positive1, dpor, program_file, schedule_file,
        build_dir / "causal_cli_p1.out", build_dir / "causal_cli_p1.err");
    (void)cli_support::run_dpor_explain(
        "--from 0 --to 5", positive2, dpor, program_file, schedule_file,
        build_dir / "causal_cli_p2.out", build_dir / "causal_cli_p2.err");
    TASK_CHECK(positive1 == positive2);
    TASK_CHECK(!cli_support::parse_cli(positive1).path.empty());
    return 0;
}

} // namespace

// task_support/driver.c is compiled as C++ by task_support/run.sh, so the
// check.h declarations and these definitions share C++ linkage.
const struct task_case task_cases[] = {
    {"identity", "basic_ids", identity_basic_ids},
    {"identity", "matches_inspect_and_owns_data",
     identity_matches_inspect_and_owns_data},
    {"identity", "empty_trace_errors", identity_empty_trace_errors},

    {"edges", "program_order", edges_program_order},
    {"edges", "mutex_handoff", edges_mutex_handoff},
    {"edges", "spawn_join", edges_spawn_join},
    {"edges", "barrier_generations", edges_barrier_generations},
    {"edges", "same_pc_loop_generation", edges_same_pc_loop_generation},
    {"edges", "condition_signal", edges_condition_signal},
    {"edges", "timed_wait_timeout_has_no_wake_edge",
     edges_timed_wait_timeout_has_no_wake_edge},
    {"edges", "timed_wait_signal_wins", edges_timed_wait_signal_wins},
    {"edges", "broadcast_wakes_all", edges_broadcast_wakes_all},
    {"edges", "explain_shortest_and_lexicographic",
     edges_explain_shortest_and_lexicographic},
    {"edges", "clock_explain_agreement_families",
     edges_clock_explain_agreement_families},
    {"edges", "reject_unsupported_and_nonsc",
     edges_reject_unsupported_and_nonsc},

    {"cancel", "positive_cap_prefix", cancel_positive_cap_prefix},
    {"cancel", "api_consumer_shape", cancel_api_consumer_shape},
    {"cancel", "zero_and_overshoot", cancel_zero_and_overshoot},
    {"cancel", "cancel_then_full_matches_fresh",
     cancel_then_full_matches_fresh},

    {"cli", "explain_matches_library", cli_explain_matches_library},
    {"cli", "stable_for_unordered_and_reruns",
     cli_stable_for_unordered_and_reruns},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
