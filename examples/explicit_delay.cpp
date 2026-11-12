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

// Explicit delay mode example.
//
// A source emits one PCM block per tick, a mixer adds the current block to its own output from a
// fixed number of ticks ago (an explicit feedback delay line), and a sink records the result. The
// delay is declared on the wire with ConnectOutToInDelayed() rather than emerging from component
// ordering. Series and Parallel ticking produce the same sequence.

#include <DSPatch.h>

#include <array>
#include <iostream>
#include <memory>
#include <vector>

using namespace DSPatch;

namespace
{

using Block = std::array<float, 8>;

// Emits blocks whose first sample is the tick index.
class BlockSource final : public Component
{
public:
    BlockSource()
    {
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const&, SignalBus& outputs ) override
    {
        Block block{};
        block[0] = static_cast<float>( _tick );
        outputs.SetValue( 0, block );
        ++_tick;
    }

private:
    int _tick = 0;
};

// output[t] = current[t] + feedback[t - delay]. An empty (or not-yet-available) feedback input is
// treated as silence, i.e. zero.
class FeedbackMixer final : public Component
{
public:
    FeedbackMixer()
    {
        SetInputCount_( 2 );   // 0: current block, 1: delayed own output
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& outputs ) override
    {
        Block result{};

        if ( auto current = inputs.GetValue<Block>( 0 ) )
        {
            result = *current;
        }
        if ( auto feedback = inputs.GetValue<Block>( 1 ) )
        {
            for ( size_t i = 0; i < result.size(); ++i )
            {
                result[i] += ( *feedback )[i];
            }
        }

        outputs.SetValue( 0, result );
    }
};

// Records the first sample of every block it receives.
class BlockSink final : public Component
{
public:
    BlockSink()
    {
        SetInputCount_( 1 );
    }

    std::vector<float> const& FirstSamples() const
    {
        return _firstSamples;
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& ) override
    {
        if ( auto block = inputs.GetValue<Block>( 0 ) )
        {
            _firstSamples.push_back( ( *block )[0] );
        }
        else
        {
            _firstSamples.push_back( 0.0f );  // empty input reads as silence
        }
    }

private:
    std::vector<float> _firstSamples;
};

std::vector<float> Run( Component::TickMode mode, int ticks, int delay )
{
    Circuit circuit;
    circuit.SetExplicitDelayMode( true );

    auto source = std::make_shared<BlockSource>();
    auto mixer = std::make_shared<FeedbackMixer>();
    auto sink = std::make_shared<BlockSink>();

    circuit.AddComponent( source );
    circuit.AddComponent( mixer );
    circuit.AddComponent( sink );

    // normal wire: this tick's source block
    circuit.ConnectOutToIn( source, 0, mixer, 0 );

    // explicit feedback: mixer input 1 sees the mixer's own output `delay` ticks ago
    if ( !circuit.ConnectOutToInDelayed( mixer, 0, mixer, 1, delay ) )
    {
        throw std::runtime_error( "delayed connection rejected" );
    }

    circuit.ConnectOutToIn( mixer, 0, sink, 0 );

    if ( !circuit.PrepareTick() )  // normal edges must still form a DAG
    {
        throw std::runtime_error( "circuit preparation failed (zero-delay cycle?)" );
    }

    for ( int i = 0; i < ticks; ++i )
    {
        circuit.Tick( mode );
    }

    return sink->FirstSamples();
}

}  // namespace

int main()
{
    int const ticks = 6;
    int const delay = 2;

    auto series = Run( Component::TickMode::Series, ticks, delay );
    auto parallel = Run( Component::TickMode::Parallel, ticks, delay );

    std::cout << "tick,series,parallel\n";
    for ( int t = 0; t < ticks; ++t )
    {
        std::cout << t << ',' << series[t] << ',' << parallel[t] << '\n';
    }

    if ( series != parallel )
    {
        std::cerr << "series and parallel sequences differ\n";
        return 1;
    }

    // reference: y[t] = t + (t >= delay ? y[t - delay] : 0)
    std::vector<float> expected( ticks, 0.0f );
    for ( int t = 0; t < ticks; ++t )
    {
        expected[t] = static_cast<float>( t ) + ( t >= delay ? expected[t - delay] : 0.0f );
    }

    if ( series != expected )
    {
        std::cerr << "sequence does not match the delay recurrence\n";
        return 1;
    }

    std::cout << "EXPLICIT_DELAY_EXAMPLE_OK\n";
    return 0;
}
