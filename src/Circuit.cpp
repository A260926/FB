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

#include <dspatch/Circuit.h>

#include <internal/AutoTickThread.h>
#include <internal/CircuitThread.h>
#include <internal/ExplicitScheduler.h>

#include <stdexcept>

using namespace DSPatch;

namespace DSPatch
{
namespace internal
{

class Circuit
{
public:
    bool FindComponent( DSPatch::Component::SCPtr const& component, int& returnIndex ) const;

    int pauseCount = 0;
    int currentThreadNo = 0;

    AutoTickThread autoTickThread;

    std::vector<DSPatch::Component::SPtr> components;

    std::vector<CircuitThread::UPtr> circuitThreads;

    // explicit delay mode state
    struct NormalWire
    {
        DSPatch::Component::SPtr fromComponent;
        int fromOutput;
        DSPatch::Component::SPtr toComponent;
        int toInput;
    };

    struct DelayedWire
    {
        DSPatch::Component::SPtr fromComponent;
        int fromOutput;
        DSPatch::Component::SPtr toComponent;
        int toInput;
        int delay;
    };

    bool explicitMode = false;
    bool prepared = false;
    std::vector<NormalWire> explicitWires;
    std::vector<DelayedWire> delayedWires;
    ExplicitScheduler explicitScheduler;

    // Moves every legacy component wire into explicitWires (and disconnects it from the legacy
    // feedback machinery), so connections made before enabling explicit mode keep working.
    void HarvestLegacyWires();

    // Re-connects the harvested normal wires through the legacy machinery and drops delayed wires.
    void RestoreLegacyWires();

    void InvalidatePreparation();
    void RemoveExplicitConnections( int componentIndex );
};

}  // namespace internal
}  // namespace DSPatch

Circuit::Circuit()
    : p( new internal::Circuit() )
{
}

Circuit::~Circuit()
{
    StopAutoTick();
    SetBufferCount( 0 );
    RemoveAllComponents();
}

int Circuit::AddComponent( Component::SPtr const& component )
{
    if ( component != nullptr )
    {
        int componentIndex;

        if ( p->FindComponent( component, componentIndex ) )
        {
            return componentIndex;  // if the component is already in the array
        }

        // components within the circuit need to have as many buffers as there are threads in the circuit
        component->SetBufferCount( (int)p->circuitThreads.size() );

        PauseAutoTick();
        p->components.emplace_back( component );
        ResumeAutoTick();

        p->InvalidatePreparation();

        return (int)p->components.size() - 1;
    }

    return -1;
}

void Circuit::RemoveComponent( Component::SCPtr const& component )
{
    int componentIndex;

    if ( p->FindComponent( component, componentIndex ) )
    {
        RemoveComponent( componentIndex );
    }
}

void Circuit::RemoveComponent( int componentIndex )
{
    PauseAutoTick();

    if ( p->explicitMode )
    {
        p->RemoveExplicitConnections( componentIndex );
    }
    else
    {
        DisconnectComponent( componentIndex );
    }

    if ( !p->components.empty() )
    {
        p->components.erase( p->components.begin() + componentIndex );
    }

    p->InvalidatePreparation();

    ResumeAutoTick();
}

void Circuit::RemoveAllComponents()
{
    for ( size_t i = 0; i < p->components.size(); ++i )
    {
        RemoveComponent( (int)i-- );  // size drops as one is removed
    }
}

// cppcheck-suppress unusedFunction
int Circuit::GetComponentCount() const
{
    return (int)p->components.size();
}

bool Circuit::ConnectOutToIn( Component::SCPtr const& fromComponent, int fromOutput, Component::SCPtr const& toComponent, int toInput )
{
    int toComponentIndex;
    int fromComponentIndex;
    if ( p->FindComponent( fromComponent, fromComponentIndex ) && p->FindComponent( toComponent, toComponentIndex ) )
    {
        return ConnectOutToIn( fromComponentIndex, fromOutput, toComponentIndex, toInput );
    }

    return false;
}

bool Circuit::ConnectOutToIn( Component::SCPtr const& fromComponent, int fromOutput, int toComponent, int toInput )
{
    int fromComponentIndex;
    if ( p->FindComponent( fromComponent, fromComponentIndex ) )
    {
        return ConnectOutToIn( fromComponentIndex, fromOutput, toComponent, toInput );
    }

    return false;
}

bool Circuit::ConnectOutToIn( int fromComponent, int fromOutput, Component::SCPtr const& toComponent, int toInput )
{
    int toComponentIndex;
    if ( p->FindComponent( toComponent, toComponentIndex ) )
    {
        return ConnectOutToIn( fromComponent, fromOutput, toComponentIndex, toInput );
    }

    return false;
}

bool Circuit::ConnectOutToIn( int fromComponent, int fromOutput, int toComponent, int toInput )
{
    if ( (size_t)fromComponent >= p->components.size() || (size_t)toComponent >= p->components.size() )
    {
        return false;
    }

    if ( p->explicitMode )
    {
        auto& from = p->components[fromComponent];
        auto& to = p->components[toComponent];

        if ( fromOutput >= from->GetOutputCount() || toInput >= to->GetInputCount() )
        {
            return false;
        }

        // each input accepts a single wire: drop any wire already connected to this input
        for ( auto it = p->explicitWires.begin(); it != p->explicitWires.end(); )
        {
            if ( it->toComponent == to && it->toInput == toInput )
            {
                it = p->explicitWires.erase( it );
            }
            else
            {
                ++it;
            }
        }
        for ( auto it = p->delayedWires.begin(); it != p->delayedWires.end(); )
        {
            if ( it->toComponent == to && it->toInput == toInput )
            {
                it = p->delayedWires.erase( it );
            }
            else
            {
                ++it;
            }
        }

        p->explicitWires.push_back( { from, fromOutput, to, toInput } );
        p->InvalidatePreparation();
        return true;
    }

    PauseAutoTick();
    bool result = p->components[toComponent]->ConnectInput( p->components[fromComponent], fromOutput, toInput );
    ResumeAutoTick();

    return result;
}

bool Circuit::ConnectOutToInDelayed( Component::SCPtr const& fromComponent, int fromOutput,
                                     Component::SCPtr const& toComponent, int toInput, int delay )
{
    int toComponentIndex;
    int fromComponentIndex;
    if ( p->FindComponent( fromComponent, fromComponentIndex ) && p->FindComponent( toComponent, toComponentIndex ) )
    {
        return ConnectOutToInDelayed( fromComponentIndex, fromOutput, toComponentIndex, toInput, delay );
    }

    return false;
}

bool Circuit::ConnectOutToInDelayed( Component::SCPtr const& fromComponent, int fromOutput, int toComponent,
                                     int toInput, int delay )
{
    int fromComponentIndex;
    if ( p->FindComponent( fromComponent, fromComponentIndex ) )
    {
        return ConnectOutToInDelayed( fromComponentIndex, fromOutput, toComponent, toInput, delay );
    }

    return false;
}

bool Circuit::ConnectOutToInDelayed( int fromComponent, int fromOutput, Component::SCPtr const& toComponent,
                                     int toInput, int delay )
{
    int toComponentIndex;
    if ( p->FindComponent( toComponent, toComponentIndex ) )
    {
        return ConnectOutToInDelayed( fromComponent, fromOutput, toComponentIndex, toInput, delay );
    }

    return false;
}

bool Circuit::ConnectOutToInDelayed( int fromComponent, int fromOutput, int toComponent, int toInput, int delay )
{
    if ( !p->explicitMode )
    {
        // delayed wires only exist in explicit delay mode
        return false;
    }

    if ( delay <= 0 || (size_t)fromComponent >= p->components.size() || (size_t)toComponent >= p->components.size() )
    {
        return false;
    }

    auto& from = p->components[fromComponent];
    auto& to = p->components[toComponent];

    if ( fromOutput >= from->GetOutputCount() || toInput >= to->GetInputCount() )
    {
        return false;
    }

    // each input accepts a single wire: drop any wire already connected to this input
    for ( auto it = p->explicitWires.begin(); it != p->explicitWires.end(); )
    {
        if ( it->toComponent == to && it->toInput == toInput )
        {
            it = p->explicitWires.erase( it );
        }
        else
        {
            ++it;
        }
    }
    for ( auto it = p->delayedWires.begin(); it != p->delayedWires.end(); )
    {
        if ( it->toComponent == to && it->toInput == toInput )
        {
            it = p->delayedWires.erase( it );
        }
        else
        {
            ++it;
        }
    }

    p->delayedWires.push_back( { from, fromOutput, to, toInput, delay } );
    p->InvalidatePreparation();
    return true;
}

void Circuit::DisconnectComponent( Component::SCPtr const& component )
{
    int componentIndex;

    if ( p->FindComponent( component, componentIndex ) )
    {
        DisconnectComponent( componentIndex );
    }
}

void Circuit::DisconnectComponent( int componentIndex )
{
    PauseAutoTick();

    if ( p->explicitMode )
    {
        p->RemoveExplicitConnections( componentIndex );
        p->InvalidatePreparation();
        ResumeAutoTick();
        return;
    }

    // remove component from _inputComponents and _inputWires
    p->components[componentIndex]->DisconnectAllInputs();

    // remove any connections this component has to other components
    for ( auto& component : p->components )
    {
        component->DisconnectInput( p->components[componentIndex] );
    }

    ResumeAutoTick();
}

void Circuit::SetBufferCount( int bufferCount )
{
    if ( p->explicitMode && bufferCount != 0 )
    {
        // explicit delay mode only supports the un-pipelined (single buffer) configuration
        throw std::runtime_error(
            "DSPatch: explicit delay mode only supports SetBufferCount(0); disable explicit mode before multi-buffering" );
    }

    if ( (size_t)bufferCount != p->circuitThreads.size() )
    {
        PauseAutoTick();

        // stop all threads
        for ( auto& circuitThread : p->circuitThreads )
        {
            circuitThread->Stop();
        }

        // resize thread array
        p->circuitThreads.resize( bufferCount );

        // initialise and start all threads
        for ( size_t i = 0; i < p->circuitThreads.size(); ++i )
        {
            if ( !p->circuitThreads[i] )
            {
                p->circuitThreads[i] = std::unique_ptr<internal::CircuitThread>( new internal::CircuitThread() );
            }
            p->circuitThreads[i]->Start( &p->components, (int)i );
        }

        // set all components to the new buffer count
        for ( auto& component : p->components )
        {
            component->SetBufferCount( bufferCount );
        }

        ResumeAutoTick();
    }
}

int Circuit::GetBufferCount() const
{
    return (int)p->circuitThreads.size();
}

void Circuit::SetExplicitDelayMode( bool enabled )
{
    if ( enabled == p->explicitMode )
    {
        return;
    }

    PauseAutoTick();

    if ( enabled )
    {
        if ( !p->circuitThreads.empty() )
        {
            ResumeAutoTick();
            throw std::runtime_error(
                "DSPatch: cannot enable explicit delay mode while multi-buffering is active; call SetBufferCount(0) first" );
        }

        p->explicitMode = true;
        p->HarvestLegacyWires();
    }
    else
    {
        p->explicitMode = false;
        p->RestoreLegacyWires();
    }

    p->InvalidatePreparation();
    ResumeAutoTick();
}

bool Circuit::GetExplicitDelayMode() const
{
    return p->explicitMode;
}

bool Circuit::PrepareTick()
{
    if ( !p->explicitMode )
    {
        return false;
    }

    std::vector<internal::ExplicitScheduler::NormalEdge> normalEdges;
    std::vector<internal::ExplicitScheduler::DelayedEdge> delayedEdges;

    normalEdges.reserve( p->explicitWires.size() );
    for ( auto const& wire : p->explicitWires )
    {
        int fromIndex;
        int toIndex;
        if ( p->FindComponent( wire.fromComponent, fromIndex ) && p->FindComponent( wire.toComponent, toIndex ) )
        {
            normalEdges.push_back( { fromIndex, wire.fromOutput, toIndex, wire.toInput } );
        }
    }

    delayedEdges.reserve( p->delayedWires.size() );
    for ( auto const& wire : p->delayedWires )
    {
        int fromIndex;
        int toIndex;
        if ( p->FindComponent( wire.fromComponent, fromIndex ) && p->FindComponent( wire.toComponent, toIndex ) )
        {
            delayedEdges.push_back( { fromIndex, wire.fromOutput, toIndex, wire.toInput, wire.delay } );
        }
    }

    if ( !p->explicitScheduler.Prepare( &p->components, normalEdges, delayedEdges ) )
    {
        // a zero-delay cycle remains: preparation fails and nothing may be processed
        p->prepared = false;
        return false;
    }

    p->prepared = true;
    return true;
}

void Circuit::Tick( Component::TickMode mode )
{
    if ( p->explicitMode )
    {
        if ( !p->prepared )
        {
            throw std::runtime_error( "DSPatch: explicit delay circuit must be prepared with PrepareTick() before Tick()" );
        }

        p->explicitScheduler.Tick( mode );
        return;
    }

    // process in a single thread if this circuit has no threads
    // =========================================================
    if ( p->circuitThreads.empty() )
    {
        // tick all internal components
        for ( auto& component : p->components )
        {
            component->Tick( mode );
        }

        // reset all internal components
        for ( auto& component : p->components )
        {
            component->Reset();
        }
    }
    // process in multiple threads if this circuit has threads
    // =======================================================
    else
    {
        p->circuitThreads[p->currentThreadNo]->SyncAndResume( mode );  // sync and resume thread x

        p->currentThreadNo = p->currentThreadNo + 1 == (int)p->circuitThreads.size() ? 0 : p->currentThreadNo + 1;
    }
}

void Circuit::StartAutoTick( Component::TickMode mode )
{
    if ( p->explicitMode )
    {
        // the explicit delay scheduler is driven manually; the auto-tick thread is not wired to it
        throw std::runtime_error( "DSPatch: auto-tick is not available in explicit delay mode; call Tick() manually" );
    }

    if ( p->autoTickThread.IsStopped() )
    {
        p->autoTickThread.Start( this, mode );
    }
    else
    {
        ResumeAutoTick();
    }
}

void Circuit::StopAutoTick()
{
    if ( !p->autoTickThread.IsStopped() )
    {
        p->autoTickThread.Stop();

        // manually tick until 0
        while ( p->currentThreadNo != 0 )
        {
            Tick( p->autoTickThread.Mode() );
        }

        // sync all threads
        for ( auto& circuitThread : p->circuitThreads )
        {
            circuitThread->Sync();
        }
    }
}

void Circuit::PauseAutoTick()
{
    if ( p->autoTickThread.IsStopped() )
    {
        return;
    }

    if ( ++p->pauseCount == 1 && !p->autoTickThread.IsPaused() )
    {
        p->autoTickThread.Pause();

        // manually tick until 0
        while ( p->currentThreadNo != 0 )
        {
            Tick( p->autoTickThread.Mode() );
        }

        // sync all threads
        for ( auto& circuitThread : p->circuitThreads )
        {
            circuitThread->Sync();
        }
    }
}

void Circuit::ResumeAutoTick()
{
    if ( p->autoTickThread.IsPaused() && --p->pauseCount == 0 )
    {
        p->autoTickThread.Resume();
    }
}

bool internal::Circuit::FindComponent( DSPatch::Component::SCPtr const& component, int& returnIndex ) const
{
    for ( size_t i = 0; i < components.size(); ++i )
    {
        if ( components[i] == component )
        {
            returnIndex = (int)i;
            return true;
        }
    }

    return false;
}

void internal::Circuit::InvalidatePreparation()
{
    prepared = false;
}

void internal::Circuit::HarvestLegacyWires()
{
    explicitWires.clear();
    delayedWires.clear();

    for ( auto& component : components )
    {
        // Component keeps its legacy wires privately; disconnect and re-record each one here
        auto wires = component->GetLegacyWires_();
        for ( auto const& wire : wires )
        {
            explicitWires.push_back( { wire.fromComponent, wire.fromOutput, component, wire.toInput } );
        }
        component->DisconnectAllInputs();
    }
}

void internal::Circuit::RestoreLegacyWires()
{
    for ( auto const& wire : explicitWires )
    {
        wire.toComponent->ConnectInput( wire.fromComponent, wire.fromOutput, wire.toInput );
    }

    explicitWires.clear();
    delayedWires.clear();
}

void internal::Circuit::RemoveExplicitConnections( int componentIndex )
{
    auto const& component = components[componentIndex];

    for ( auto it = explicitWires.begin(); it != explicitWires.end(); )
    {
        if ( it->toComponent == component || it->fromComponent == component )
        {
            it = explicitWires.erase( it );
        }
        else
        {
            ++it;
        }
    }

    for ( auto it = delayedWires.begin(); it != delayedWires.end(); )
    {
        if ( it->toComponent == component || it->fromComponent == component )
        {
            it = delayedWires.erase( it );
        }
        else
        {
            ++it;
        }
    }
}
