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

#include <internal/ExplicitScheduler.h>

#include <algorithm>
#include <queue>
#include <stdexcept>

using namespace DSPatch;
using namespace DSPatch::internal;

ExplicitScheduler::ExplicitScheduler()
{
}

ExplicitScheduler::~ExplicitScheduler()
{
    _ResetWorkers();
}

bool ExplicitScheduler::Prepare( std::vector<DSPatch::Component::SPtr>* components, std::vector<NormalEdge> const& normalEdges,
                                 std::vector<DelayedEdge> const& delayedEdges )
{
    _components = components;
    _normalEdges = normalEdges;
    _delayedEdges = delayedEdges;
    _levels.clear();
    _incoming.clear();
    _lastNormalEdge.clear();
    _history.clear();
    _pending.clear();
    _tickCount = 0;
    _prepared = false;

    int const componentCount = (int)_components->size();

    _incoming.resize( componentCount );

    // build the normal-edge adjacency graph
    std::vector<std::vector<int>> adjacency( componentCount );
    std::vector<int> indegrees( componentCount, 0 );

    for ( int edgeIndex = 0; edgeIndex < (int)_normalEdges.size(); ++edgeIndex )
    {
        auto const& edge = _normalEdges[edgeIndex];
        adjacency[edge.fromComponent].push_back( edge.toComponent );
        ++indegrees[edge.toComponent];
        _incoming[edge.toComponent].push_back( edgeIndex );
    }

    // Kahn topological sort, grouped into dependency levels
    std::vector<int> componentLevel( componentCount, -1 );
    std::queue<int> ready;

    for ( int i = 0; i < componentCount; ++i )
    {
        if ( indegrees[i] == 0 )
        {
            ready.push( i );
            componentLevel[i] = 0;
        }
    }

    int visited = 0;

    while ( !ready.empty() )
    {
        int componentIndex = ready.front();
        ready.pop();
        ++visited;

        int level = componentLevel[componentIndex];
        if ( (int)_levels.size() <= level )
        {
            _levels.resize( level + 1 );
        }
        _levels[level].push_back( componentIndex );

        for ( int toComponent : adjacency[componentIndex] )
        {
            componentLevel[toComponent] = std::max( componentLevel[toComponent], level + 1 );
            if ( --indegrees[toComponent] == 0 )
            {
                ready.push( toComponent );
            }
        }
    }

    if ( visited != componentCount )
    {
        // normal edges contain a cycle: refuse to prepare, nothing has been processed
        _levels.clear();
        _incoming.clear();
        return false;
    }

    // The final normal consumer of each output moves the signal; that consumer must be the last
    // one visited, so pick the edge landing in the deepest level (edge order breaks ties).
    for ( int edgeIndex = 0; edgeIndex < (int)_normalEdges.size(); ++edgeIndex )
    {
        auto const& edge = _normalEdges[edgeIndex];
        auto key = std::make_pair( edge.fromComponent, edge.fromOutput );
        auto current = _lastNormalEdge.find( key );
        if ( current == _lastNormalEdge.end() ||
             componentLevel[_normalEdges[current->second].toComponent] <= componentLevel[edge.toComponent] )
        {
            _lastNormalEdge[key] = edgeIndex;
        }
    }

    // one history ring per source output that delayed edges read from
    for ( auto const& edge : _delayedEdges )
    {
        auto key = std::make_pair( edge.fromComponent, edge.fromOutput );
        auto ring = _history.find( key );
        if ( ring == _history.end() )
        {
            ring = _history.emplace( key, HistoryRing() ).first;
        }
        ring->second.maxDelay = std::max( ring->second.maxDelay, edge.delay );
    }

    _prepared = true;
    return true;
}

bool ExplicitScheduler::IsPrepared() const
{
    return _prepared;
}

void ExplicitScheduler::Tick( DSPatch::Component::TickMode mode )
{
    if ( !_prepared )
    {
        throw std::runtime_error( "DSPatch: explicit delay circuit must be prepared (PrepareTick()) before Tick()" );
    }

    // 1. clear every input bus, then fix all delayed inputs before any new value is produced
    for ( auto& component : *_components )
    {
        component->ExplicitClearInputs_();
    }

    for ( auto const& edge : _delayedEdges )
    {
        auto const& ring = _history.at( std::make_pair( edge.fromComponent, edge.fromOutput ) );

        // the newest ring entry is the output from tick (_tickCount - 1); delay d asks for tick (_tickCount - d)
        int ringIndex = (int)ring.entries.size() - edge.delay;
        if ( ringIndex >= 0 && ring.entries[ringIndex].hasValue )
        {
            ( *_components )[edge.toComponent]->ExplicitInputBus_().CopySignal(
                edge.toInput, ring.entries[ringIndex].signal );
        }
        // t < d, or the source wrote nothing on that tick: the input stays empty (no value is repeated)
    }

    // 2. run every component exactly once, level by level
    for ( auto const& level : _levels )
    {
        _TransferInputs( level );
        _RunProcesses( level, mode );
        _StageOutputs( level );
    }

    // 3. publish this tick's staged outputs into the delay histories, only after the whole tick is done
    _PublishHistory();

    ++_tickCount;
}

void ExplicitScheduler::_TransferInputs( std::vector<int> const& level )
{
    // All sources sit in earlier levels and have finished. The final normal consumer of an output
    // moves its signal, so every copy in this level is performed first; the move edges run in a
    // second pass. Consumers in earlier levels have already been served, so no copy can starve.
    for ( int pass = 0; pass < 2; ++pass )
    {
        for ( int componentIndex : level )
        {
            for ( int edgeIndex : _incoming[componentIndex] )
            {
                auto const& edge = _normalEdges[edgeIndex];
                auto key = std::make_pair( edge.fromComponent, edge.fromOutput );
                bool isMove = _lastNormalEdge.at( key ) == edgeIndex;
                if ( isMove != ( pass == 1 ) )
                {
                    continue;
                }

                auto& sourceBus = ( *_components )[edge.fromComponent]->ExplicitOutputBus_();
                auto& targetBus = ( *_components )[componentIndex]->ExplicitInputBus_();

                if ( !sourceBus.HasValue( edge.fromOutput ) )
                {
                    continue;  // the source wrote nothing this tick: the target input stays empty
                }

                if ( isMove )
                {
                    targetBus.MoveSignal( edge.toInput, sourceBus.GetSignal( edge.fromOutput ) );
                }
                else
                {
                    targetBus.CopySignal( edge.toInput, sourceBus.GetSignal( edge.fromOutput ) );
                }
            }
        }
    }
}

void ExplicitScheduler::_RunProcesses( std::vector<int> const& level, DSPatch::Component::TickMode mode )
{
    if ( mode == DSPatch::Component::TickMode::Series )
    {
        for ( int componentIndex : level )
        {
            ( *_components )[componentIndex]->ExplicitClearOutputs_();
            ( *_components )[componentIndex]->ExplicitProcess_();
        }
        return;
    }

    if ( _workers.empty() )
    {
        unsigned int workerCount = std::thread::hardware_concurrency();
        if ( workerCount < 2 )
        {
            workerCount = 2;
        }

        _stopping = false;
        for ( unsigned int i = 0; i < workerCount; ++i )
        {
            _workers.emplace_back( [this]() {
                while ( true )
                {
                    std::function<void()> job;
                    {
                        std::unique_lock<std::mutex> lock( _jobMutex );
                        _jobCondt.wait( lock, [this]() { return _stopping || _jobsRemaining > 0; } );

                        if ( _stopping )
                        {
                            return;
                        }

                        int jobIndex = (int)_jobs.size() - _jobsRemaining;
                        --_jobsRemaining;
                        job = _jobs[jobIndex];
                    }
                    job();
                    {
                        std::lock_guard<std::mutex> lock( _jobMutex );
                        ++_jobsDone;
                        if ( _jobsDone == (int)_jobs.size() )
                        {
                            _doneCondt.notify_all();
                        }
                    }
                }
            } );
        }
    }

    {
        std::unique_lock<std::mutex> lock( _jobMutex );
        _jobs.clear();
        for ( int componentIndex : level )
        {
            _jobs.push_back( [this, componentIndex]() {
                ( *_components )[componentIndex]->ExplicitClearOutputs_();
                ( *_components )[componentIndex]->ExplicitProcess_();
            } );
        }
        _jobsRemaining = (int)_jobs.size();
        _jobsDone = 0;
        _jobCondt.notify_all();
        _doneCondt.wait( lock, [this]() { return _jobsDone == (int)_jobs.size(); } );
    }
}

void ExplicitScheduler::_StageOutputs( std::vector<int> const& level )
{
    // Snapshot history-tracked outputs after the level's processes have all completed and before
    // any later level can move these signals away.
    for ( int componentIndex : level )
    {
        for ( auto const& historyEntry : _history )
        {
            if ( historyEntry.first.first != componentIndex )
            {
                continue;
            }

            int outputIndex = historyEntry.first.second;
            HistoryEntry entry;
            auto const& sourceSignal = ( *_components )[componentIndex]->ExplicitOutputBus_().GetSignal( outputIndex );
            if ( sourceSignal != nullptr && sourceSignal->HasValue() )
            {
                entry.hasValue = true;
                entry.signal = std::make_shared<Signal>();
                entry.signal->CopySignal( sourceSignal );  // independent deep copy
            }
            _pending[std::make_pair( componentIndex, outputIndex )] = std::move( entry );
        }
    }
}

void ExplicitScheduler::_PublishHistory()
{
    for ( auto& ringEntry : _history )
    {
        auto pending = _pending.find( ringEntry.first );
        if ( pending != _pending.end() )
        {
            ringEntry.second.entries.push_back( std::move( pending->second ) );
        }
        else
        {
            ringEntry.second.entries.push_back( HistoryEntry() );  // component produced nothing this tick
        }

        while ( (int)ringEntry.second.entries.size() > ringEntry.second.maxDelay )
        {
            ringEntry.second.entries.pop_front();
        }
    }
    _pending.clear();
}

void ExplicitScheduler::_ResetWorkers()
{
    if ( _workers.empty() )
    {
        return;
    }

    {
        std::lock_guard<std::mutex> lock( _jobMutex );
        _stopping = true;
        _jobCondt.notify_all();
    }

    for ( auto& worker : _workers )
    {
        worker.join();
    }
    _workers.clear();
}
