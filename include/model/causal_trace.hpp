#pragma once

// Causal evidence graph for DPOR schedule replay.
//
// The public types (model::CausalEvent, model::CausalEdge, model::CausalTrace)
// own all of their data and never reference interpreter state. Graph edges
// are not inferred from vector-clock comparability: the production interpreter
// records an edge (or a deferred provenance source) at every point where it
// actually merges a synchronization clock. See src/model/causal_trace.cpp and
// the instrumentation hooks in src/model/checker.cpp.

#include "model/checker.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace model {

struct CausalEvent {
    // Zero-based commit ordinal, also the index into CausalTrace::events.
    std::size_t id{0};
    ScheduleStep endpoint{};
    Action effective_action{};
    // Dense snapshot of the committing thread's clock immediately after the
    // production interpreter committed this action. Component order is thread
    // id and the vector has one entry per program thread.
    std::vector<std::uint64_t> clock_after;
    // Absolute cyclic-barrier generation the event participated in, using the
    // interpreter's zero-based ordinal. Engaged only for BarrierWait events.
    std::optional<std::uint64_t> barrier_generation;
    // Exact park/timeout occurrence for TimedWait source endpoints. Reacquire
    // commits materialize as an effective Lock and therefore leave this empty.
    std::optional<TimedWaitOccurrence> timed_wait_occurrence;

    bool operator==(const CausalEvent&) const = default;
};

struct CausalEdge {
    std::size_t from{0};
    std::size_t to{0};
    // Stable, non-empty human-readable provenance label, e.g. "program-order",
    // "mutex", "spawn", "join", "barrier", "condition-signal", ...
    std::string reason;

    bool operator==(const CausalEdge&) const = default;
};

struct CausalTrace {
    std::vector<CausalEvent> events;
    std::vector<CausalEdge> edges;
    // True only when a positive max_events cap stopped the replay while
    // uncommitted schedule steps remained.
    bool cancelled{false};

    // Indices into `edges` forming a directed evidence path from event `from`
    // to event `to`. Returns an empty vector for a self query or when no
    // strict happens-before relation is evidenced. Throws std::out_of_range
    // when either id is not an event id. When several paths exist, selects by
    // fewest edges, then lexicographically smallest event-id sequence.
    [[nodiscard]] std::vector<std::size_t> explain(std::size_t from,
                                                   std::size_t to) const;
};

namespace causal_detail {

// One out-of-band clock merge into a thread that has not committed since the
// merge happened (spawn publication, a condition-variable wake performed by
// another thread, or a barrier release applied to a parked participant).
struct PendingProvenance {
    std::size_t source{0};
    std::string reason;
};

struct BarrierArrivalRecord {
    ThreadId thread{0};
    std::size_t event{0};
};

// Mutable recorder driven by the production interpreter. All methods are
// inline in this header so checker.o stays self-contained in build modes that
// do not link src/model/causal_trace.cpp (notably task_support baseline).
class CausalRecorder {
public:
    CausalTrace trace;
    std::size_t current{0};
    ThreadId current_thread{0};

    void initialize(std::size_t thread_count) {
        last_event.assign(thread_count, std::nullopt);
        pending.assign(thread_count, {});
    }

    // Opens the next dynamic event. Assigns its commit id, records the
    // program-order edge from the thread's previous event, and anchors every
    // deferred merge source on this commit.
    std::size_t begin_commit(ThreadId tid,
                             ScheduleStep endpoint,
                             Action effective_action,
                             std::optional<std::uint64_t> barrier_generation,
                             std::optional<TimedWaitOccurrence> occurrence) {
        current = trace.events.size();
        current_thread = tid;

        CausalEvent event;
        event.id = current;
        event.endpoint = endpoint;
        event.effective_action = std::move(effective_action);
        event.barrier_generation = barrier_generation;
        event.timed_wait_occurrence = occurrence;
        trace.events.push_back(std::move(event));

        if (tid < last_event.size() && last_event[tid].has_value()) {
            add_edge(*last_event[tid], current, "program-order");
        }
        if (tid < pending.size()) {
            for (const PendingProvenance& provenance : pending[tid]) {
                add_edge(provenance.source, current, provenance.reason);
            }
            pending[tid].clear();
        }
        if (tid < last_event.size()) {
            last_event[tid] = current;
        }
        return current;
    }

    void finish_commit(std::size_t id, std::vector<std::uint64_t> clock_after) {
        trace.events.at(id).clock_after = std::move(clock_after);
    }

    void add_edge(std::size_t from, std::size_t to, std::string reason) {
        trace.edges.push_back(CausalEdge{from, to, std::move(reason)});
    }

    // The committing thread joins a mutex release frontier.
    void mutex_acquire(const std::string& mutex_name) {
        const auto found = mutex_release_event.find(mutex_name);
        if (found == mutex_release_event.end()) {
            return;
        }
        add_edge(found->second,
                 current,
                 "mutex acquire '" + mutex_name + "'");
    }

    // An unlock (or a Wait park releasing the mutex) publishes this commit's
    // clock as the mutex frontier.
    void mutex_release(const std::string& mutex_name) {
        mutex_release_event[mutex_name] = current;
    }

    // Spawn joins the spawning commit's clock into the new thread before its
    // first commit; anchor the edge on that first commit.
    void spawned(ThreadId child) {
        if (child < pending.size()) {
            pending[child].push_back(
                PendingProvenance{current, "spawn target thread " + std::to_string(child)});
        }
    }

    // Join acquires the target thread's whole clock frontier. Its latest
    // commit anchors the frontier; any out-of-band merges not yet observed by a
    // target commit are acquired directly by this join.
    void joined(ThreadId target) {
        if (target < last_event.size() && last_event[target].has_value()) {
            add_edge(*last_event[target],
                     current,
                     "join thread " + std::to_string(target));
        }
        if (target < pending.size()) {
            for (const PendingProvenance& provenance : pending[target]) {
                add_edge(provenance.source, current, provenance.reason);
            }
        }
    }

    // Signal/Broadcast merged the waker's (current commit's) clock into a
    // parked waiter; anchor the evidence on the waiter's reacquire commit.
    void woke(ThreadId waiter, const std::string& reason) {
        if (waiter < pending.size()) {
            pending[waiter].push_back(PendingProvenance{current, reason});
        }
    }

    void barrier_arrival(const std::string& barrier_name,
                         std::uint64_t generation,
                         ThreadId tid) {
        auto& records = barrier_arrivals[barrier_key(barrier_name, generation)];
        records.push_back(BarrierArrivalRecord{tid, current});
    }

    // The last participant completed the generation. The committing
    // participant observes the release clock on this event; every other
    // participant observes it at their next commit.
    void barrier_released(const std::string& barrier_name,
                          std::uint64_t generation,
                          const std::vector<ThreadId>& participants) {
        const std::string key = barrier_key(barrier_name, generation);
        const auto found = barrier_arrivals.find(key);
        const std::vector<BarrierArrivalRecord> empty;
        const std::vector<BarrierArrivalRecord>& arrivals =
            found == barrier_arrivals.end() ? empty : found->second;

        const std::string reason =
            "barrier '" + barrier_name + "' generation " + std::to_string(generation);

        for (const ThreadId participant : participants) {
            for (const BarrierArrivalRecord& arrival : arrivals) {
                if (arrival.thread == participant) {
                    // The participant's own arrival is already ordered by
                    // program order; never emit a self edge.
                    continue;
                }
                if (participant == current_thread) {
                    add_edge(arrival.event, current, reason);
                } else if (participant < pending.size()) {
                    pending[participant].push_back(
                        PendingProvenance{arrival.event, reason});
                }
            }
        }
        barrier_arrivals.erase(key);
    }

private:
    std::vector<std::optional<std::size_t>> last_event;
    std::vector<std::vector<PendingProvenance>> pending;
    std::map<std::string, std::size_t> mutex_release_event;
    std::map<std::string, std::vector<BarrierArrivalRecord>> barrier_arrivals;

    static std::string barrier_key(const std::string& name, std::uint64_t generation) {
        return name + "#" + std::to_string(generation);
    }
};

// Entry point implemented in src/model/checker.cpp because it must drive the
// production interpreter and nothing else.
CausalTrace run_causal_trace(const Program& program,
                             const Schedule& schedule,
                             std::size_t step_bound,
                             MemoryModel memory_model,
                             std::size_t max_events);

} // namespace causal_detail

} // namespace model
