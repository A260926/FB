#include "suite.h"

#include <DSPatch.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <vector>

using namespace DSPatch;

namespace
{

using Block = std::array<float, 8>;

// ------------------------------------------------------------------------------------------------
// Test components
// ------------------------------------------------------------------------------------------------

struct IntSource : Component
{
    int value;
    int step;
    int ticks = 0;

    IntSource( int start = 0, int increment = 1 )
        : value( start )
        , step( increment )
    {
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const&, SignalBus& outputs ) override
    {
        outputs.SetValue( 0, value );
        value += step;
        ++ticks;
    }
};

struct Adder2 : Component
{
    Adder2()
    {
        SetInputCount_( 2 );
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& outputs ) override
    {
        auto a = inputs.GetValue<int>( 0 );
        auto b = inputs.GetValue<int>( 1 );
        outputs.SetValue( 0, ( a ? *a : 0 ) + ( b ? *b : 0 ) );
    }
};

struct Adder3 : Component
{
    Adder3()
    {
        SetInputCount_( 3 );
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& outputs ) override
    {
        auto a = inputs.GetValue<int>( 0 );
        auto b = inputs.GetValue<int>( 1 );
        auto c = inputs.GetValue<int>( 2 );
        outputs.SetValue( 0, ( a ? *a : 0 ) + ( b ? *b : 0 ) + ( c ? *c : 0 ) );
    }
};

struct Pass : Component
{
    Pass()
    {
        SetInputCount_( 1 );
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& outputs ) override
    {
        auto in = inputs.GetValue<int>( 0 );
        if ( in )
        {
            outputs.SetValue( 0, *in );
        }
    }
};

struct IntRecorder : Component
{
    std::vector<int> seen;
    std::vector<char> has;

    IntRecorder()
    {
        SetInputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& ) override
    {
        auto p = inputs.GetValue<int>( 0 );
        has.push_back( p != nullptr );
        seen.push_back( p ? *p : 0 );
    }
};

struct BlockSource : Component
{
    int ticks = 0;

    BlockSource()
    {
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const&, SignalBus& outputs ) override
    {
        Block b{};
        for ( int i = 0; i < 8; ++i )
        {
            b[i] = float( ticks * 8 + i );
        }
        outputs.SetValue( 0, b );
        ++ticks;
    }
};

struct SporadicBlockSource : Component
{
    int ticks = 0;

    SporadicBlockSource()
    {
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const&, SignalBus& outputs ) override
    {
        if ( ticks % 2 == 0 )  // writes only on even ticks
        {
            Block b{};
            b[0] = float( ticks );
            b[7] = float( ticks ) + 0.5f;
            outputs.SetValue( 0, b );
        }
        ++ticks;
    }
};

struct TypeChangeSource : Component
{
    int ticks = 0;

    TypeChangeSource()
    {
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const&, SignalBus& outputs ) override
    {
        switch ( ticks % 3 )
        {
            case 0:
            {
                Block b{};
                b[0] = float( ticks );
                outputs.SetValue( 0, b );
                break;
            }
            case 1:
                outputs.SetValue( 0, 1000 + ticks );
                break;
            case 2:
                break;  // deliberately writes nothing
        }
        ++ticks;
    }
};

struct BlockRecorder : Component
{
    std::vector<Block> vals;
    std::vector<char> has;

    BlockRecorder()
    {
        SetInputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& ) override
    {
        auto p = inputs.GetValue<Block>( 0 );
        has.push_back( p != nullptr );
        vals.push_back( p ? *p : Block{} );
    }
};

struct TypeRecorder : Component
{
    std::vector<std::type_index> types;
    std::vector<char> has;
    std::vector<int> intVals;
    std::vector<Block> blockVals;

    TypeRecorder()
    {
        SetInputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& ) override
    {
        bool present = inputs.HasValue( 0 );
        has.push_back( present );
        types.push_back( present ? std::type_index( inputs.GetType( 0 ) ) : std::type_index( typeid( void ) ) );

        auto ip = inputs.GetValue<int>( 0 );
        intVals.push_back( ip ? *ip : -1 );

        auto bp = inputs.GetValue<Block>( 0 );
        blockVals.push_back( bp ? *bp : Block{} );
    }
};

// Mutates its own input signal in place, then records and forwards it.
struct BlockMutator : Component
{
    float add;
    Block last{};
    std::vector<Block> all;
    bool got = false;

    BlockMutator( float add_ )
        : add( add_ )
    {
        SetInputCount_( 1 );
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const& inputs, SignalBus& outputs ) override
    {
        auto p = inputs.GetValue<Block>( 0 );
        if ( p )
        {
            Block b = *p;
            for ( float& x : b )
            {
                x += add;
            }
            inputs.GetSignal( 0 )->SetValue( b );  // mutate this component's own input
            last = b;
            all.push_back( b );
            got = true;
            outputs.SetValue( 0, b );
        }
    }
};

struct CountingComponent : Component
{
    int runs = 0;

    CountingComponent( int inputs, int outputs )
    {
        SetInputCount_( inputs );
        SetOutputCount_( outputs );
    }

protected:
    void Process_( SignalBus const&, SignalBus& ) override
    {
        ++runs;
    }
};

// Reusable barrier with a timed wait, built from a condition variable.
struct TimedBarrier
{
    explicit TimedBarrier( std::size_t parties_ )
        : parties( parties_ )
    {
    }

    bool ArriveAndWait( std::chrono::milliseconds timeout )
    {
        std::unique_lock<std::mutex> lock( mutex );
        std::size_t generation = generationCount;
        if ( ++waiting == parties )
        {
            waiting = 0;
            ++generationCount;
            cond.notify_all();
            return true;
        }
        return cond.wait_for( lock, timeout, [&]() { return generationCount != generation; } );
    }

    std::mutex mutex;
    std::condition_variable cond;
    std::size_t parties;
    std::size_t waiting = 0;
    std::size_t generationCount = 0;
};

struct BarrierComponent : Component
{
    TimedBarrier* barrier;
    bool overlapped = false;
    int runs = 0;

    BarrierComponent( TimedBarrier* barrier_ )
        : barrier( barrier_ )
    {
        SetOutputCount_( 1 );
    }

protected:
    void Process_( SignalBus const&, SignalBus& ) override
    {
        ++runs;
        overlapped = barrier->ArriveAndWait( std::chrono::milliseconds( 2000 ) );
    }
};

Block MakeBlock( int tick )
{
    Block b{};
    for ( int i = 0; i < 8; ++i )
    {
        b[i] = float( tick * 8 + i );
    }
    return b;
}

bool TickThrows( Circuit& circuit, Component::TickMode mode )
{
    try
    {
        circuit.Tick( mode );
    }
    catch ( std::exception const& )
    {
        return true;
    }
    return false;
}

// Self-loop y_t = x_t + y_{t-1}, built with the components added in a chosen order.
std::vector<int> RunSelfLoop( Component::TickMode mode, bool reversed, int ticks )
{
    Circuit circuit;
    circuit.SetExplicitDelayMode( true );

    auto src = std::make_shared<IntSource>( 0, 1 );
    auto adder = std::make_shared<Adder2>();
    auto rec = std::make_shared<IntRecorder>();

    if ( reversed )
    {
        circuit.AddComponent( rec );
        circuit.AddComponent( adder );
        circuit.AddComponent( src );
    }
    else
    {
        circuit.AddComponent( src );
        circuit.AddComponent( adder );
        circuit.AddComponent( rec );
    }

    UNIT_CHECK( circuit.ConnectOutToIn( src, 0, adder, 0 ) );
    UNIT_CHECK( circuit.ConnectOutToInDelayed( adder, 0, adder, 1, 1 ) );
    UNIT_CHECK( circuit.ConnectOutToIn( adder, 0, rec, 0 ) );
    UNIT_CHECK( circuit.PrepareTick() );

    for ( int i = 0; i < ticks; ++i )
    {
        circuit.Tick( mode );
    }
    return rec->seen;
}

}  // namespace

// =================================================================================================
// recurrence group: independent, per-tick array recurrences
// =================================================================================================

UNIT_TEST( "recurrence", self_loop_delay_one )
{
    int const ticks = 8;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<IntSource>( 0, 1 );  // x_t = t
        auto adder = std::make_shared<Adder2>();
        auto rec = std::make_shared<IntRecorder>();

        circuit.AddComponent( src );
        circuit.AddComponent( adder );
        circuit.AddComponent( rec );

        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, adder, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToInDelayed( adder, 0, adder, 1, 1 ) );  // y_{t-1}
        UNIT_CHECK( circuit.ConnectOutToIn( adder, 0, rec, 0 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        // independent reference: y_t = t + y_{t-1}, y_{-1} = 0  ->  triangular numbers
        std::vector<int> expected( ticks );
        int previous = 0;
        for ( int t = 0; t < ticks; ++t )
        {
            int y = t + ( t >= 1 ? previous : 0 );
            expected[t] = y;
            previous = y;
        }

        UNIT_CHECK( rec->seen == expected );
        UNIT_CHECK( rec->has == std::vector<char>( ticks, 1 ) );
        UNIT_CHECK( expected[3] == 6 && expected[4] == 10 );  // 0,1,3,6,10,...
    }
}

UNIT_TEST( "recurrence", double_delay_ring )
{
    int const ticks = 8;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto srcA = std::make_shared<IntSource>( 1, 0 );  // a_t = 1
        auto srcB = std::make_shared<IntSource>( 2, 0 );  // b_t = 2
        auto a = std::make_shared<Adder2>();
        auto b = std::make_shared<Adder2>();
        auto recA = std::make_shared<IntRecorder>();
        auto recB = std::make_shared<IntRecorder>();

        for ( auto& c : { std::static_pointer_cast<Component>( srcA ), std::static_pointer_cast<Component>( srcB ),
                          std::static_pointer_cast<Component>( a ), std::static_pointer_cast<Component>( b ),
                          std::static_pointer_cast<Component>( recA ), std::static_pointer_cast<Component>( recB ) } )
        {
            circuit.AddComponent( c );
        }

        UNIT_CHECK( circuit.ConnectOutToIn( srcA, 0, a, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( srcB, 0, b, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToInDelayed( b, 0, a, 1, 1 ) );  // A reads B from t-1
        UNIT_CHECK( circuit.ConnectOutToInDelayed( a, 0, b, 1, 2 ) );  // B reads A from t-2
        UNIT_CHECK( circuit.ConnectOutToIn( a, 0, recA, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( b, 0, recB, 0 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        // independent arrays, honouring empty inputs for t < delay
        std::vector<int> y( ticks ), z( ticks );
        for ( int t = 0; t < ticks; ++t )
        {
            y[t] = 1 + ( t >= 1 ? z[t - 1] : 0 );
            z[t] = 2 + ( t >= 2 ? y[t - 2] : 0 );
        }

        UNIT_CHECK( recA->seen == y );
        UNIT_CHECK( recB->seen == z );
        UNIT_CHECK( y[0] == 1 && z[0] == 2 && y[1] == 3 && z[2] == 3 );
    }
}

UNIT_TEST( "recurrence", feed_forward_diamond )
{
    int const ticks = 6;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<IntSource>( 10, 1 );  // s_t = 10 + t
        auto p1 = std::make_shared<Pass>();
        auto p2 = std::make_shared<Pass>();
        auto merge = std::make_shared<Adder2>();
        auto rec = std::make_shared<IntRecorder>();

        circuit.AddComponent( src );
        circuit.AddComponent( p1 );
        circuit.AddComponent( p2 );
        circuit.AddComponent( merge );
        circuit.AddComponent( rec );

        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, p1, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, p2, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( p1, 0, merge, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( p2, 0, merge, 1 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( merge, 0, rec, 0 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        std::vector<int> expected( ticks );
        for ( int t = 0; t < ticks; ++t )
        {
            expected[t] = 2 * ( 10 + t );
        }

        UNIT_CHECK( rec->seen == expected );
    }
}

// =================================================================================================
// ownership group: fan-out, empty blocks, type changes, independent input copies
// =================================================================================================

UNIT_TEST( "ownership", fanout_move_keeps_delay_history )
{
    int const ticks = 6;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<BlockSource>();
        auto normal = std::make_shared<BlockRecorder>();  // sole normal consumer: moves the signal
        auto d1 = std::make_shared<BlockRecorder>();
        auto d3 = std::make_shared<BlockRecorder>();

        circuit.AddComponent( src );
        circuit.AddComponent( normal );
        circuit.AddComponent( d1 );
        circuit.AddComponent( d3 );

        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, normal, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, d1, 0, 1 ) );
        UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, d3, 0, 3 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        UNIT_CHECK( normal->has == std::vector<char>( ticks, 1 ) );
        UNIT_CHECK( d1->has == ( std::vector<char>{ 0, 1, 1, 1, 1, 1 } ) );
        UNIT_CHECK( d3->has == ( std::vector<char>{ 0, 0, 0, 1, 1, 1 } ) );

        for ( int t = 0; t < ticks; ++t )
        {
            UNIT_CHECK( normal->vals[t] == MakeBlock( t ) );
            if ( t >= 1 )
            {
                UNIT_CHECK( d1->vals[t] == MakeBlock( t - 1 ) );
            }
            if ( t >= 3 )
            {
                UNIT_CHECK( d3->vals[t] == MakeBlock( t - 3 ) );
            }
        }
    }
}

UNIT_TEST( "ownership", empty_blocks_are_not_repeated )
{
    int const ticks = 8;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<SporadicBlockSource>();
        auto d1 = std::make_shared<BlockRecorder>();

        circuit.AddComponent( src );
        circuit.AddComponent( d1 );

        UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, d1, 0, 1 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        // the source writes on even ticks, so the delay-1 input is present only on odd ticks
        std::vector<char> expectedHas( ticks, 0 );
        for ( int t = 1; t < ticks; ++t )
        {
            expectedHas[t] = ( ( t - 1 ) % 2 == 0 ) ? 1 : 0;
        }

        UNIT_CHECK( d1->has == expectedHas );
        for ( int t = 0; t < ticks; ++t )
        {
            if ( expectedHas[t] )
            {
                // the sporadic source stamps its source tick into b[0]; the value must come from t-1
                UNIT_CHECK( d1->vals[t][0] == float( t - 1 ) );
                UNIT_CHECK( d1->vals[t][7] == float( t - 1 ) + 0.5f );
            }
        }
    }
}

UNIT_TEST( "ownership", delayed_line_preserves_type_changes )
{
    int const ticks = 9;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<TypeChangeSource>();
        auto d1 = std::make_shared<TypeRecorder>();

        circuit.AddComponent( src );
        circuit.AddComponent( d1 );

        UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, d1, 0, 1 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        std::vector<char> expectedHas( ticks, 0 );
        std::vector<std::type_index> expectedTypes( ticks, std::type_index( typeid( void ) ) );
        for ( int t = 1; t < ticks; ++t )
        {
            int sourceTick = t - 1;
            int kind = sourceTick % 3;
            expectedHas[t] = ( kind == 2 ) ? 0 : 1;
            expectedTypes[t] = ( kind == 0 ) ? std::type_index( typeid( Block ) )
                             : ( kind == 1 ) ? std::type_index( typeid( int ) )
                                             : std::type_index( typeid( void ) );
        }

        UNIT_CHECK( d1->has == expectedHas );
        UNIT_CHECK( d1->types == expectedTypes );
        UNIT_CHECK( d1->intVals[2] == 1001 );   // source tick 1 emitted int 1001
        UNIT_CHECK( d1->blockVals[1][0] == 0.f );  // source tick 0 emitted a Block whose [0] is 0
    }
}

UNIT_TEST( "ownership", mutating_one_input_does_not_pollute_other_lines )
{
    int const ticks = 5;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<BlockSource>();
        auto m1 = std::make_shared<BlockMutator>( 1.0f );
        auto m2 = std::make_shared<BlockMutator>( 100.0f );
        auto history = std::make_shared<BlockRecorder>();  // delay-1 view must stay pristine

        circuit.AddComponent( src );
        circuit.AddComponent( m1 );
        circuit.AddComponent( m2 );
        circuit.AddComponent( history );

        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, m1, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, m2, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, history, 0, 1 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }

        UNIT_CHECK( m1->got && m2->got );
        UNIT_CHECK( (int)m1->all.size() == ticks && (int)m2->all.size() == ticks );

        for ( int t = 0; t < ticks; ++t )
        {
            Block base = MakeBlock( t );
            for ( int i = 0; i < 8; ++i )
            {
                UNIT_CHECK( m1->all[t][i] == base[i] + 1.0f );    // not +101
                UNIT_CHECK( m2->all[t][i] == base[i] + 100.0f );  // not +101
            }
            if ( t >= 1 )
            {
                UNIT_CHECK( history->vals[t] == MakeBlock( t - 1 ) );  // deep-copied history is unmodified
            }
        }
    }
}

// =================================================================================================
// scheduling group: ordering, cycle rejection, equivalence, real parallel overlap, mode rules
// =================================================================================================

UNIT_TEST( "scheduling", add_order_does_not_change_results )
{
    int const ticks = 10;

    for ( auto mode : { Component::TickMode::Series, Component::TickMode::Parallel } )
    {
        auto forward = RunSelfLoop( mode, false, ticks );
        auto reversed = RunSelfLoop( mode, true, ticks );

        UNIT_CHECK( forward.size() == (size_t)ticks );
        UNIT_CHECK( forward == reversed );
        UNIT_CHECK( forward[9] == 45 );  // triangular number for t=9
    }
}

UNIT_TEST( "scheduling", zero_delay_cycle_is_rejected_without_processing )
{
    Circuit circuit;
    circuit.SetExplicitDelayMode( true );

    auto a = std::make_shared<CountingComponent>( 1, 1 );
    auto b = std::make_shared<CountingComponent>( 1, 1 );

    circuit.AddComponent( a );
    circuit.AddComponent( b );

    UNIT_CHECK( circuit.ConnectOutToIn( a, 0, b, 0 ) );
    UNIT_CHECK( circuit.ConnectOutToIn( b, 0, a, 0 ) );

    UNIT_CHECK( !circuit.PrepareTick() );  // zero-delay cycle
    UNIT_CHECK( a->runs == 0 && b->runs == 0 );
    UNIT_CHECK( TickThrows( circuit, Component::TickMode::Series ) );
    UNIT_CHECK( TickThrows( circuit, Component::TickMode::Parallel ) );

    // replacing one edge with a delayed edge breaks the zero-delay cycle
    UNIT_CHECK( circuit.ConnectOutToInDelayed( b, 0, a, 0, 1 ) );
    UNIT_CHECK( circuit.PrepareTick() );
    circuit.Tick( Component::TickMode::Series );
    UNIT_CHECK( a->runs == 1 && b->runs == 1 );  // each component runs exactly once per tick
}

UNIT_TEST( "scheduling", series_and_parallel_produce_the_same_sequence )
{
    int const ticks = 12;

    auto run = [&]( Component::TickMode mode ) {
        Circuit circuit;
        circuit.SetExplicitDelayMode( true );

        auto src = std::make_shared<IntSource>( 1, 1 );
        auto p1 = std::make_shared<Pass>();
        auto p2 = std::make_shared<Pass>();
        auto merge = std::make_shared<Adder3>();  // diamond + self feedback
        auto rec = std::make_shared<IntRecorder>();

        circuit.AddComponent( src );
        circuit.AddComponent( p1 );
        circuit.AddComponent( p2 );
        circuit.AddComponent( merge );
        circuit.AddComponent( rec );

        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, p1, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( src, 0, p2, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( p1, 0, merge, 0 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( p2, 0, merge, 1 ) );
        UNIT_CHECK( circuit.ConnectOutToInDelayed( merge, 0, merge, 2, 1 ) );
        UNIT_CHECK( circuit.ConnectOutToIn( merge, 0, rec, 0 ) );
        UNIT_CHECK( circuit.PrepareTick() );

        for ( int i = 0; i < ticks; ++i )
        {
            circuit.Tick( mode );
        }
        return rec->seen;
    };

    auto series = run( Component::TickMode::Series );
    auto parallel = run( Component::TickMode::Parallel );

    std::vector<int> expected( ticks );
    int previous = 0;
    for ( int t = 0; t < ticks; ++t )
    {
        int m = 2 * ( 1 + t ) + ( t >= 1 ? previous : 0 );
        expected[t] = m;
        previous = m;
    }

    UNIT_CHECK( series == parallel );
    UNIT_CHECK( series == expected );
}

UNIT_TEST( "scheduling", independent_nodes_really_overlap_in_parallel )
{
    Circuit circuit;
    circuit.SetExplicitDelayMode( true );

    TimedBarrier barrier( 2 );
    auto x = std::make_shared<BarrierComponent>( &barrier );
    auto y = std::make_shared<BarrierComponent>( &barrier );

    circuit.AddComponent( x );
    circuit.AddComponent( y );
    UNIT_CHECK( circuit.PrepareTick() );  // no wires: both nodes share the first dependency level

    circuit.Tick( Component::TickMode::Parallel );

    UNIT_CHECK( x->overlapped && y->overlapped );  // barrier can only complete if both run concurrently
    UNIT_CHECK( x->runs == 1 && y->runs == 1 );

    // series mode still produces a valid single tick (the timed barrier is not used as a gate here)
}

UNIT_TEST( "scheduling", prepare_buffer_and_rewiring_rules )
{
    Circuit circuit;
    circuit.SetExplicitDelayMode( true );

    auto src = std::make_shared<IntSource>( 0, 1 );
    auto rec = std::make_shared<IntRecorder>();
    circuit.AddComponent( src );
    circuit.AddComponent( rec );

    UNIT_CHECK( TickThrows( circuit, Component::TickMode::Series ) );  // not prepared

    UNIT_CHECK( !circuit.ConnectOutToInDelayed( src, 0, rec, 0, 0 ) );   // delay must be positive
    UNIT_CHECK( !circuit.ConnectOutToInDelayed( src, 0, rec, 0, -2 ) );

    UNIT_CHECK( circuit.ConnectOutToIn( src, 0, rec, 0 ) );
    UNIT_CHECK( circuit.PrepareTick() );
    circuit.Tick( Component::TickMode::Series );
    UNIT_CHECK( rec->seen == std::vector<int>{ 0 } );

    // rewiring invalidates the preparation
    UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, rec, 0, 1 ) );
    UNIT_CHECK( TickThrows( circuit, Component::TickMode::Series ) );
    UNIT_CHECK( circuit.PrepareTick() );
    circuit.Tick( Component::TickMode::Series );  // tick 0 again: the delay line is empty
    UNIT_CHECK( rec->has == ( std::vector<char>{ 1, 0 } ) );

    // explicit mode is limited to the un-pipelined configuration and manual ticking
    bool bufferRejected = false;
    try
    {
        circuit.SetBufferCount( 2 );
    }
    catch ( std::exception const& )
    {
        bufferRejected = true;
    }
    UNIT_CHECK( bufferRejected );
    UNIT_CHECK( circuit.GetBufferCount() == 0 );
    circuit.SetBufferCount( 0 );  // the supported value stays accepted

    bool autoTickRejected = false;
    try
    {
        circuit.StartAutoTick();
    }
    catch ( std::exception const& )
    {
        autoTickRejected = true;
    }
    UNIT_CHECK( autoTickRejected );
}

UNIT_TEST( "scheduling", repreparing_clears_history_and_restarts_at_tick_zero )
{
    Circuit circuit;
    circuit.SetExplicitDelayMode( true );

    auto src = std::make_shared<IntSource>( 7, 1 );
    auto d1 = std::make_shared<IntRecorder>();

    circuit.AddComponent( src );
    circuit.AddComponent( d1 );

    UNIT_CHECK( circuit.ConnectOutToInDelayed( src, 0, d1, 0, 1 ) );
    UNIT_CHECK( circuit.PrepareTick() );

    for ( int i = 0; i < 4; ++i )
    {
        circuit.Tick( Component::TickMode::Series );
    }
    UNIT_CHECK( d1->has == ( std::vector<char>{ 0, 1, 1, 1 } ) );
    UNIT_CHECK( d1->seen == ( std::vector<int>{ 0, 7, 8, 9 } ) );

    UNIT_CHECK( circuit.PrepareTick() );  // histories must be cleared, ticking restarts at 0

    circuit.Tick( Component::TickMode::Series );
    UNIT_CHECK( d1->has == ( std::vector<char>{ 0, 1, 1, 1, 0 } ) );  // first tick after re-prepare is empty
}

UNIT_TEST( "scheduling", wiring_before_enabling_mode_is_adopted )
{
    Circuit circuit;

    auto src = std::make_shared<IntSource>( 3, 1 );
    auto rec = std::make_shared<IntRecorder>();
    circuit.AddComponent( src );
    circuit.AddComponent( rec );

    UNIT_CHECK( circuit.ConnectOutToIn( src, 0, rec, 0 ) );  // legacy wire first
    circuit.SetExplicitDelayMode( true );
    UNIT_CHECK( circuit.GetExplicitDelayMode() );
    UNIT_CHECK( circuit.PrepareTick() );

    for ( int i = 0; i < 3; ++i )
    {
        circuit.Tick( Component::TickMode::Parallel );
    }
    UNIT_CHECK( rec->seen == ( std::vector<int>{ 3, 4, 5 } ) );
}
