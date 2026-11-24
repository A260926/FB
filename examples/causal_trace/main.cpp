// Causal evidence graph example: a cyclic barrier used for two generations.
//
// Two workers take a mutex-protected step, synchronize at barrier "phase",
// yield, and synchronize at the same barrier again. The graph keeps the two
// barrier generations apart even though both waits share one source PC.

#include "model/causal_trace.hpp"
#include "model/checker.hpp"

#include <cstddef>
#include <iostream>
#include <string>

using model::Action;
using model::ActionKind;
using model::CausalTrace;
using model::ModelChecker;
using model::Program;
using model::Schedule;
using model::ScheduleStep;

namespace {

Action action(ActionKind kind) {
    Action result;
    result.kind = kind;
    return result;
}

void print_path(const CausalTrace& trace, std::size_t from, std::size_t to) {
    const std::vector<std::size_t> path = trace.explain(from, to);
    std::cout << "explain " << from << " -> " << to << ":";
    if (path.empty()) {
        std::cout << " no strict happens-before path\n";
        return;
    }

    std::cout << " events " << from;
    for (const std::size_t edge_index : path) {
        std::cout << " -> " << trace.edges[edge_index].to;
    }
    std::cout << '\n';
    for (const std::size_t edge_index : path) {
        const auto& edge = trace.edges[edge_index];
        std::cout << "    edge[" << edge_index << "] " << edge.from << " -> "
                  << edge.to << "  " << edge.reason << '\n';
    }
}

} // namespace

int main() {
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

    Program program{{
        {lock, write, unlock, barrier, yield, barrier},
        {lock, write, unlock, barrier, yield, barrier},
    }};

    // Thread 0 completes its first critical section and parks at the first
    // barrier, then thread 1 does the same and releases generation 0. The
    // trailing yield/barrier pairs close a second, independent generation.
    Schedule schedule;
    for (std::uint32_t i = 0; i < 4; ++i) {
        schedule.push_back(ScheduleStep{0, i, std::nullopt});
    }
    for (std::uint32_t i = 0; i < 4; ++i) {
        schedule.push_back(ScheduleStep{1, i, std::nullopt});
    }
    schedule.push_back(ScheduleStep{0, 4, std::nullopt});
    schedule.push_back(ScheduleStep{0, 5, std::nullopt});
    schedule.push_back(ScheduleStep{1, 4, std::nullopt});
    schedule.push_back(ScheduleStep{1, 5, std::nullopt});

    ModelChecker checker(program);
    const CausalTrace trace = checker.causal_trace(schedule);

    if (trace.cancelled || trace.events.size() != 12) {
        std::cerr << "unexpected causal replay state\n";
        return 1;
    }

    std::cout << "cyclic barrier causal trace: " << trace.events.size()
              << " events, " << trace.edges.size() << " evidence edges\n";

    for (const auto& event : trace.events) {
        std::cout << "event " << event.id << " thread " << event.endpoint.thread
                  << " action_index " << event.endpoint.action_index;
        if (event.barrier_generation.has_value()) {
            std::cout << " barrier_generation=" << *event.barrier_generation;
        }
        std::cout << " clock_after=[";
        for (std::size_t i = 0; i < event.clock_after.size(); ++i) {
            if (i != 0) {
                std::cout << ',';
            }
            std::cout << event.clock_after[i];
        }
        std::cout << "]\n";
    }

    std::cout << "\n-- generation 0 barrier evidence --\n";
    print_path(trace, 3, 7);

    std::cout << "\n-- mutex-protected write happens-before write --\n";
    print_path(trace, 1, 5);

    std::cout << "\n-- generation 1 barrier evidence (same source PC) --\n";
    print_path(trace, 9, 11);

    // Both generations must be observed and must stay distinct.
    bool saw_generation_zero = false;
    bool saw_generation_one = false;
    for (const auto& event : trace.events) {
        if (event.barrier_generation == std::optional<std::uint64_t>{0}) {
            saw_generation_zero = true;
        }
        if (event.barrier_generation == std::optional<std::uint64_t>{1}) {
            saw_generation_one = true;
        }
    }
    if (!saw_generation_zero || !saw_generation_one ||
        trace.explain(9, 11).empty() || trace.explain(3, 7).empty()) {
        std::cerr << "barrier generation evidence missing\n";
        return 1;
    }

    std::cout << "\nOK: both barrier generations explained independently\n";
    return 0;
}
