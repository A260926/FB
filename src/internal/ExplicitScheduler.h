/******************************************************************************
DSPatch - The Refreshingly Simple C++ Dataflow Framework
Copyright (c) 2022, Marcus Tomlinson

BSD 2-Clause License

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#pragma once

#include <dspatch/Common.h>
#include <dspatch/Component.h>
#include <dspatch/Signal.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace DSPatch
{
namespace internal
{

/// Explicit-delay scheduler

/**
Drives a Circuit in explicit delay mode. Normal (zero-delay) wires must form a DAG once the delayed
wires are removed; Prepare() verifies this with a Kahn topological sort and groups components into
dependency levels. Every Tick() runs each component exactly once, level by level. In Parallel mode
the Process_() calls within a level are dispatched to a persistent worker pool, so components with
no normal dependency on each other genuinely overlap.

Signal transfers for a whole level are performed on the scheduling thread before that level's
components process: all sources live in earlier levels and are already finished, so fan-out
copy/move never races (the last normal consumer of an output moves it). Workers therefore only ever
touch their own component's buses.

Delayed wires never touch Component's legacy wire / feedback machinery. Their values are served
from per-output delay histories holding independent deep copies of past signals. Values staged
during a tick only enter the histories once every component of that tick has finished.
*/
class ExplicitScheduler final
{
public:
    NONCOPYABLE( ExplicitScheduler );

    struct NormalEdge
    {
        int fromComponent;
        int fromOutput;
        int toComponent;
        int toInput;
    };

    struct DelayedEdge
    {
        int fromComponent;
        int fromOutput;
        int toComponent;
        int toInput;
        int delay;  // positive number of ticks
    };

    ExplicitScheduler();
    ~ExplicitScheduler();

    /// Builds the level schedule from the normal edges and resets all delay histories.
    /// Returns false (without running any component) if the normal edges contain a cycle.
    bool Prepare( std::vector<DSPatch::Component::SPtr>* components, std::vector<NormalEdge> const& normalEdges,
                  std::vector<DelayedEdge> const& delayedEdges );

    bool IsPrepared() const;

    /// Runs one tick. Throws std::runtime_error if Prepare() has not been called (or was invalidated).
    void Tick( DSPatch::Component::TickMode mode );

private:
    struct HistoryEntry
    {
        bool hasValue = false;
        Signal::SPtr signal;  // independent deep copy of the source output
    };

    struct HistoryRing
    {
        int maxDelay = 0;
        std::deque<HistoryEntry> entries;  // oldest at the front, newest at the back
    };

    void _ResetWorkers();
    void _TransferInputs( std::vector<int> const& level );
    void _RunProcesses( std::vector<int> const& level, DSPatch::Component::TickMode mode );
    void _StageOutputs( std::vector<int> const& level );
    void _PublishHistory();

    std::vector<DSPatch::Component::SPtr>* _components = nullptr;
    std::vector<NormalEdge> _normalEdges;
    std::vector<DelayedEdge> _delayedEdges;

    std::vector<std::vector<int>> _incoming;  // normal edge indices feeding each component
    std::vector<std::vector<int>> _levels;    // topological levels of component indices
    std::map<std::pair<int, int>, int> _lastNormalEdge;  // source output -> index of its final normal edge

    std::map<std::pair<int, int>, HistoryRing> _history;
    std::map<std::pair<int, int>, HistoryEntry> _pending;  // staged during a tick, published at its end

    bool _prepared = false;
    unsigned long long _tickCount = 0;

    // worker pool
    std::vector<std::thread> _workers;
    std::mutex _jobMutex;
    std::condition_variable _jobCondt, _doneCondt;
    std::vector<std::function<void()>> _jobs;
    int _jobsRemaining = 0;
    int _jobsDone = 0;
    bool _stopping = false;
};

}  // namespace internal
}  // namespace DSPatch
