[![Build & Test](https://github.com/cross-platform/dspatch/actions/workflows/build_and_test.yml/badge.svg)](https://github.com/cross-platform/dspatch/actions/workflows/build_and_test.yml)

# DSPatch

The Refreshingly Simple C++ Dataflow Framework

Webite: http://flowbasedprogramming.com

DSPatch, pronounced "dispatch", is a powerful C++ dataflow framework. DSPatch is not limited to any particular domain or data type, from reactive programming to stream processing, DSPatch's generic, object-oriented API allows you to create virtually any dataflow system imaginable.


## Build

```
git clone https://github.com/cross-platform/dspatch.git
cd dspatch
mkdir build
cd build
cmake ..
make
```

- *`cmake ..` will auto-detect your IDE / compiler. To manually select one, use `cmake -G`.*
- *When building for an IDE, instead of `make`, simply open the cmake generated project file.*


## Explicit delay mode

The default circuit behaviour breaks a feedback loop wherever the recursive `Tick()` first meets a
component that has already started ticking. That yields a one-block delay whose position depends on
the order components were added to the circuit. For feedback effects such as delay lines you often
want to state the delay on the wire instead. `Circuit` therefore offers an independent, explicit
scheduling mode that leaves the original behaviour untouched.

Enable it on a circuit, declare any delayed wires, then prepare and tick:

```cpp
#include <DSPatch.h>

using namespace DSPatch;

Circuit circuit;
circuit.SetExplicitDelayMode( true );          // switch to explicit scheduling

auto source = std::make_shared<Source>();      // produces std::array<float, 8> blocks
auto feedback = std::make_shared<FeedbackNode>();
auto sink = std::make_shared<Sink>();

circuit.AddComponent( source );
circuit.AddComponent( feedback );
circuit.AddComponent( sink );

// ordinary wire: the input receives THIS tick's signal
circuit.ConnectOutToIn( source, 0, feedback, 0 );

// delayed wire: input 1 receives the output produced `ticks` blocks ago (ticks must be > 0)
circuit.ConnectOutToInDelayed( feedback, 0, feedback, 1, 2 );  // a 2-block feedback delay
circuit.ConnectOutToIn( feedback, 0, sink, 0 );

if ( !circuit.PrepareTick() )                  // builds the schedule; false = zero-delay cycle
{
    // the normal (non-delayed) wires contain a cycle. No component is processed in this case.
}

for ( int tick = 0; tick < 100; ++tick )
{
    circuit.Tick( Component::TickMode::Series );   // TickMode::Parallel gives the same sequence
}
```

### Interface

- `SetExplicitDelayMode(bool)` / `GetExplicitDelayMode()` — turn the mode on or off. Existing
  connections are adopted when the mode is enabled.
- `ConnectOutToIn(...)` — unchanged: a normal wire carrying the current tick's value.
- `ConnectOutToInDelayed(fromComponent, fromOutput, toComponent, toInput, delay)` — the target
  input receives the source output produced `delay` ticks ago. `delay` must be a positive integer.
  The four pointer/index overloads mirror `ConnectOutToIn`. Only available in explicit mode.
- `PrepareTick()` — validates and compiles the schedule. With the delayed edges removed, the
  remaining wires must form a directed acyclic graph; otherwise it returns `false` and nothing is
  processed. Every successful preparation clears all delay histories and restarts ticking at
  tick 0. It must be called again after any change to the wiring.
- `Tick(mode)` — runs every component exactly once per tick. Calling it before a successful
  `PrepareTick()` (or after rewiring without re-preparing) throws `std::runtime_error`.

### Semantics and limits

- On tick `t`, an input on a wire of delay `d` receives the signal the source produced during its
  `Process_()` on tick `t - d`. For `t < d` the input is empty. If the source wrote nothing on that
  earlier tick, the input is empty too — a previous block is never repeated.
- All delayed inputs are fixed at the start of the tick, before any component runs. New outputs
  enter the delay histories only after the whole tick has completed.
- Signals keep the dynamic `SignalBus` typing (`std::array<float, 8>`, `int`, …) and may change
  type or be absent from tick to tick. Delay histories hold independent deep copies, so a consumer
  mutating its own input cannot affect another wire or the stored history.
- `TickMode::Series` and `TickMode::Parallel` produce identical sequences. In parallel mode,
  components without a normal dependency on each other still execute concurrently; the next tick's
  history is never published before all parallel work has finished.
- Only the un-pipelined configuration is supported: `SetBufferCount(0)`. Enabling multi-buffering
  while in explicit mode, or enabling the mode while multi-buffering is active, throws instead of
  falling back to the legacy feedback path. The auto-tick thread is not used; drive the circuit
  with manual `Tick()` calls. Graph changes are made between ticks, not concurrently with
  processing.

### See also:

DSPatchables (https://github.com/cross-platform/dspatchables): A DSPatch component repository.

DSPatcher (https://github.com/cross-platform/dspatcher): A cross-platform graphical tool for building DSPatch circuits.
