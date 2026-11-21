// Tests for the deferred (non-blocking) FrameGraph execution session.
//
// The CPU texture below implements the real resource interface
// (Desc/create/destroy and the optional preRead/preWrite hooks); completion
// handles are released by hand so the order in which device work finishes is
// fully controlled. Nothing relies on wall-clock timing.

#include "fg/FrameGraph.hpp"
#include "task_support/check.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Trace {
  std::vector<std::string> events;

  void add(std::string event) { events.emplace_back(std::move(event)); }

  [[nodiscard]] int count(const std::string &event) const {
    return static_cast<int>(
      std::count(events.cbegin(), events.cend(), event));
  }
  [[nodiscard]] int position(const std::string &event) const {
    const auto it = std::find(events.cbegin(), events.cend(), event);
    return it == events.cend() ? -1 : static_cast<int>(it - events.cbegin());
  }
  [[nodiscard]] bool has(const std::string &event) const {
    return position(event) >= 0;
  }
};

// A CPU texture satisfying the FrameGraph resource concept.
struct CpuTexture {
  struct Desc {
    Trace *trace;
    const char *name;
  };

  int value{0};
  bool alive{false};

  void create(const Desc &desc, void *) {
    alive = true;
    value = 0;
    desc.trace->add(std::string{"create:"} + desc.name);
  }
  void destroy(const Desc &desc, void *) {
    alive = false;
    desc.trace->add(std::string{"destroy:"} + desc.name);
  }
  void preRead(const Desc &desc, uint32_t flags, void *) {
    desc.trace->add(std::string{"preRead:"} + desc.name + ":" +
                    std::to_string(flags));
  }
  void preWrite(const Desc &desc, uint32_t flags, void *) {
    desc.trace->add(std::string{"preWrite:"} + desc.name + ":" +
                    std::to_string(flags));
  }
};

// Manually released completion token.
struct Gate {
  Trace *trace{nullptr};
  std::string name;
  bool ready{false};
  int queries{0};

  void release() {
    ready = true;
    trace->add("release:" + name);
  }
  void rearm() {
    ready = false;
    queries = 0;
  }
  [[nodiscard]] FrameGraphCompletion handle() {
    return FrameGraphCompletion{[this] {
      ++queries;
      return ready;
    }};
  }
};

struct PassData {
  FrameGraphResource texture{};
};

template <typename F> bool throwsLogicError(F &&f) {
  try {
    std::forward<F>(f)();
  } catch (const std::logic_error &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

} // namespace

//
// readiness
//

static int readinessStartDefersSubmission() {
  Trace trace;
  Gate gate{&trace, "A"};
  FrameGraph fg;

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:A");
      return gate.handle();
    });
  fg.compile();

  // start only opens the session, it must not submit anything.
  fg.startExecution();
  TASK_CHECK(fg.isExecuting());
  TASK_CHECK(trace.count("submit:A") == 0);
  TASK_CHECK(trace.count("create:x") == 0);

  // The first poll submits and queries the (not ready) handle once.
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(gate.queries == 1);

  // Still not ready: polled again, exactly once.
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(gate.queries == 2);

  gate.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  TASK_CHECK(trace.position("destroy:x") > trace.position("submit:A"));
  return 0;
}

static int readinessBlockedLaterIndependent() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  Gate c{&trace, "C"};
  FrameGraph fg;

  FrameGraphResource x{};
  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:A");
      return a.handle();
    });
  // B is blocked by A, C is independent and must still be submitted.
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:B");
      return b.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "C",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("y", {&trace, "y"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:C");
      return c.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("submit:B") == 0);
  TASK_CHECK(trace.count("submit:C") == 1);
  TASK_CHECK(trace.position("submit:A") < trace.position("submit:C"));

  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:B") == 1);

  b.release();
  c.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  TASK_CHECK(trace.count("destroy:y") == 1);
  return 0;
}

static int readinessImmediateUnlock() {
  Trace trace;
  FrameGraph fg;
  FrameGraphResource x{};

  // A plain synchronous pass inside a session: it returns no handle, so its
  // default completion is immediate and may unlock B during the same poll.
  fg.addCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("ran:A");
    });
  fg.addCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("ran:B");
    });
  fg.compile();
  fg.startExecution();

  // A single poll submits A, observes its immediate completion and unlocks B.
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("ran:A") == 1);
  TASK_CHECK(trace.count("ran:B") == 1);
  TASK_CHECK(trace.position("ran:A") < trace.position("ran:B"));
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("destroy:x") == 1);
  return 0;
}

static int readinessSingleQueryPerPoll() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  FrameGraph fg;
  for (const auto *name : {"A", "B"}) {
    const bool first = name[0] == 'A';
    fg.addAsyncCallbackPass<PassData>(
      name,
      [&](FrameGraph::Builder &builder, PassData &data) {
        data.texture =
          builder.create<CpuTexture>(first ? "x" : "y",
                                     {&trace, first ? "x" : "y"});
        data.texture = builder.write(data.texture);
        builder.setSideEffect();
      },
      [&, first](const PassData &, FrameGraphPassResources &, void *) {
        trace.add(first ? "submit:A" : "submit:B");
        return first ? a.handle() : b.handle();
      });
  }
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(a.queries == 1);
  TASK_CHECK(b.queries == 1);

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(a.queries == 2);
  TASK_CHECK(b.queries == 2);

  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(a.queries == 3); // One final query observed completion.
  TASK_CHECK(b.queries == 3);

  // A completed handle is never queried again.
  b.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(a.queries == 3);
  TASK_CHECK(b.queries == 4);
  return 0;
}

static int readinessRejectsMisuse() {
  Trace trace;

  // poll without start.
  {
    FrameGraph fg;
    fg.addAsyncCallbackPass<PassData>(
      "A",
      [&](FrameGraph::Builder &, PassData &) {},
      [&](const PassData &, FrameGraphPassResources &, void *) {
        return FrameGraphCompletion{};
      });
    fg.compile();
    TASK_CHECK(throwsLogicError([&] { (void)fg.pollExecution(); }));
  }

  // start before compile.
  {
    FrameGraph fg;
    fg.addAsyncCallbackPass<PassData>(
      "A",
      [&](FrameGraph::Builder &, PassData &) {},
      [&](const PassData &, FrameGraphPassResources &, void *) {
        return FrameGraphCompletion{};
      });
    TASK_CHECK(throwsLogicError([&] { fg.startExecution(); }));
  }

  // execute() on a graph containing async passes is rejected.
  {
    Gate gate{&trace, "A"};
    FrameGraph fg;
    fg.addAsyncCallbackPass<PassData>(
      "A",
      [&](FrameGraph::Builder &builder, PassData &data) {
        data.texture = builder.create<CpuTexture>("a", {&trace, "a"});
        data.texture = builder.write(data.texture);
        builder.setSideEffect();
      },
      [&](const PassData &, FrameGraphPassResources &, void *) {
        trace.add("submit:A");
        return gate.handle();
      });
    fg.compile();
    TASK_CHECK(throwsLogicError([&] { fg.execute(); }));
    TASK_CHECK(trace.count("submit:A") == 0);

    fg.startExecution();
    TASK_CHECK(trace.count("submit:A") == 0);
    TASK_CHECK(!fg.pollExecution());
    TASK_CHECK(trace.count("submit:A") == 1);

    // Nested start and execute while a session is running are rejected before
    // any side effect.
    TASK_CHECK(throwsLogicError([&] { fg.startExecution(); }));
    TASK_CHECK(throwsLogicError([&] { fg.execute(); }));
    TASK_CHECK(trace.count("submit:A") == 1);

    gate.release();
    TASK_CHECK(fg.pollExecution());
  }

  // A purely synchronous graph still runs through the legacy execute().
  {
    Trace sync;
    FrameGraph fg;
    fg.addCallbackPass<PassData>(
      "S",
      [&](FrameGraph::Builder &builder, PassData &data) {
        data.texture = builder.create<CpuTexture>("s", {&sync, "s"});
        data.texture = builder.write(data.texture);
        builder.setSideEffect();
      },
      [&](const PassData &, FrameGraphPassResources &, void *) {
        sync.add("ran:S");
      });
    fg.compile();
    fg.execute();
    TASK_CHECK(sync.count("ran:S") == 1);
    TASK_CHECK(sync.count("create:s") == 1);
    TASK_CHECK(sync.count("destroy:s") == 1);
  }

  // The same compiled graph may be started again after a full session.
  {
    Gate restart{&trace, "restart"};
    FrameGraph fg;
    fg.addAsyncCallbackPass<PassData>(
      "A",
      [&](FrameGraph::Builder &builder, PassData &data) {
        data.texture = builder.create<CpuTexture>("r", {&trace, "r"});
        data.texture = builder.write(data.texture);
        builder.setSideEffect();
      },
      [&](const PassData &, FrameGraphPassResources &, void *) {
        trace.add("submit:restart");
        return restart.handle();
      });
    fg.compile();
    fg.startExecution();
    TASK_CHECK(!fg.pollExecution());
    restart.release();
    TASK_CHECK(fg.pollExecution());
    TASK_CHECK(trace.count("submit:restart") == 1);
    TASK_CHECK(trace.count("create:r") == 1);
    TASK_CHECK(trace.count("destroy:r") == 1);

    restart.rearm();
    fg.startExecution();
    TASK_CHECK(!fg.pollExecution());
    TASK_CHECK(trace.count("submit:restart") == 2);
    TASK_CHECK(trace.count("create:r") == 2);
    restart.release();
    TASK_CHECK(fg.pollExecution());
    TASK_CHECK(trace.count("destroy:r") == 2);
  }
  return 0;
}

//
// hazards
//

static int hazardsWriteAfterReaders() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate r1{&trace, "R1"};
  Gate r2{&trace, "R2"};
  Gate w{&trace, "W"};
  FrameGraph fg;
  FrameGraphResource x{};
  CpuTexture *readObjectA{nullptr};
  CpuTexture *readObjectB{nullptr};
  CpuTexture *writeObject{nullptr};
  int observed{0};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      texture.value = 10;
      trace.add("submit:A");
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "R1",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      readObjectA = &resources.get<CpuTexture>(data.texture);
      trace.add("submit:R1");
      return r1.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "R2",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      readObjectB = &resources.get<CpuTexture>(data.texture);
      trace.add("submit:R2");
      return r2.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "W",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.write(x); // Renamed handle, same physical texture.
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      writeObject = &texture;
      texture.value += 1;
      observed = texture.value;
      trace.add("submit:W");
      return w.handle();
    });
  fg.compile();
  fg.startExecution();

  // Only the producer can start.
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("submit:R1") == 0);
  TASK_CHECK(trace.count("submit:R2") == 0);
  TASK_CHECK(trace.count("submit:W") == 0);

  // Both readers may overlap.
  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:R1") == 1);
  TASK_CHECK(trace.count("submit:R2") == 1);
  TASK_CHECK(trace.count("submit:W") == 0);

  // The writer waits for every reader, not just the last registered one.
  r1.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:W") == 0);
  r2.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:W") == 1);

  // Renamed handles still refer to the very same T object.
  TASK_CHECK(readObjectA == readObjectB);
  TASK_CHECK(readObjectA == writeObject);

  w.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(observed == 11);
  TASK_CHECK(trace.count("destroy:x") == 1);
  TASK_CHECK(trace.position("submit:W") > trace.position("submit:R1"));
  TASK_CHECK(trace.position("submit:W") > trace.position("submit:R2"));
  return 0;
}

static int hazardsReadAfterWrite() {
  Trace trace;
  Gate reader{&trace, "R"};
  Gate writer{&trace, "W"};
  FrameGraph fg;
  CpuTexture back;
  back.alive = true;
  back.value = 5;
  const auto x =
    fg.import("x", CpuTexture::Desc{&trace, "x"}, std::move(back));

  fg.addAsyncCallbackPass<PassData>(
    "R",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      trace.add("submit:R:" + std::to_string(texture.value));
      return reader.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "W",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.write(x);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      texture.value += 1;
      trace.add("submit:W");
      return writer.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:R:5") == 1);
  TASK_CHECK(trace.count("submit:W") == 0);
  reader.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:W") == 1);
  writer.release();
  TASK_CHECK(fg.pollExecution());
  // Imported resources are never created or destroyed.
  TASK_CHECK(trace.count("create:x") == 0);
  TASK_CHECK(trace.count("destroy:x") == 0);
  return 0;
}

static int hazardsRenamedHandleSharesObject() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  CpuTexture *first{nullptr};
  CpuTexture *second{nullptr};
  FrameGraph fg;
  FrameGraphResource x{};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      first = &texture;
      texture.value = 3;
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.write(x); // Clones the handle.
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      second = &texture;
      texture.value *= 4;
      return b.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:B") == 0);
  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(second == first);
  TASK_CHECK(second->value == 12);
  b.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("destroy:x") == 1);
  return 0;
}

static int hazardsIndependentResources() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  FrameGraph fg;

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:A");
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("y", {&trace, "y"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:B");
      return b.handle();
    });
  fg.compile();
  fg.startExecution();

  // No full-graph barrier: both independent passes are submitted at once.
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("submit:B") == 1);

  // Finishing A releases x without waiting for B's device work.
  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  TASK_CHECK(trace.count("destroy:y") == 0);

  b.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:y") == 1);
  return 0;
}

static int hazardsOutOfOrderCompletion() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  FrameGraph fg;
  FrameGraphResource x{};
  int finalValue{-1};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      resources.get<CpuTexture>(data.texture).value = 7;
      trace.add("submit:A");
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.write(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      texture.value += 5;
      finalValue = texture.value;
      trace.add("submit:B");
      return b.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  // Releasing B early does nothing: it has not been submitted yet.
  b.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:B") == 0);
  TASK_CHECK(trace.count("submit:A") == 1);

  a.release();
  // B was already released, so as soon as A completes B is submitted and its
  // completion is observed within the same poll.
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("submit:B") == 1);
  TASK_CHECK(finalValue == 12);
  TASK_CHECK(trace.count("destroy:x") == 1);
  return 0;
}

static int hazardsReadModifyWriteChain() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  FrameGraph fg;
  FrameGraphResource x{};
  CpuTexture *object{nullptr};
  bool sameObject{false};
  int observedA{-1};
  int observedB{-1};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      object = &texture;
      texture.value = 2;
      observedA = texture.value;
      trace.add("submit:A");
      return a.handle();
    });
  // B both reads and writes the same physical resource, producing a renamed
  // handle: it must still conflict with A and reuse the same T object.
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      builder.read(x, 11);
      data.texture = builder.write(x, 22);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      sameObject = &texture == object;
      texture.value *= 5;
      observedB = texture.value;
      trace.add("submit:B");
      return b.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("submit:B") == 0);
  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:B") == 1);
  TASK_CHECK(trace.count("preRead:x:11") == 1);
  TASK_CHECK(trace.count("preWrite:x:22") == 1);
  TASK_CHECK(observedA == 2);
  TASK_CHECK(observedB == 10);
  TASK_CHECK(sameObject);
  b.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("destroy:x") == 1);
  return 0;
}

static int hazardsCreateDependency() {
  Trace trace;
  Gate producer{&trace, "P"};
  Gate reader{&trace, "R"};
  FrameGraph fg;
  FrameGraphResource x{};
  int observed{-1};

  // A pass that only creates a resource (kept alive by its side effect) still
  // orders a later reader: creation is a write of the initial version.
  fg.addAsyncCallbackPass<PassData>(
    "P",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = x;
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:P");
      return producer.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "R",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      observed = resources.get<CpuTexture>(data.texture).value;
      trace.add("submit:R");
      return reader.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:P") == 1);
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("submit:R") == 0);

  producer.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:R") == 1);
  TASK_CHECK(observed == 0);
  reader.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  return 0;
}

static int readinessMixedSyncAndAsync() {
  Trace trace;
  Gate asyncGate{&trace, "async"};
  FrameGraph fg;
  FrameGraphResource x{};
  int observed{-1};

  // A synchronous pass may feed an asynchronous one inside a session; the
  // synchronous callback's return counts as an immediate completion.
  fg.addCallbackPass<PassData>(
    "sync",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      resources.get<CpuTexture>(data.texture).value = 6;
      trace.add("run:sync");
    });
  fg.addAsyncCallbackPass<PassData>(
    "async",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      observed = resources.get<CpuTexture>(data.texture).value;
      trace.add("submit:async");
      return asyncGate.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("run:sync") == 1);
  TASK_CHECK(trace.count("submit:async") == 1);
  TASK_CHECK(observed == 6);
  TASK_CHECK(trace.count("destroy:x") == 0);

  asyncGate.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  return 0;
}

//
// lifetime
//

static int lifetimeCreateDestroyOnce() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  Gate c{&trace, "C"};
  FrameGraph fg;
  FrameGraphResource x{};
  FrameGraphResource y{};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:A");
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(x);
      y = builder.create<CpuTexture>("y", {&trace, "y"});
      data.texture = builder.write(y);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:B");
      return b.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "C",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(y);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:C");
      return c.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("create:y") == 0);

  a.release();
  TASK_CHECK(!fg.pollExecution());
  // x's last use (B) has been submitted but not completed: keep x alive.
  TASK_CHECK(trace.count("destroy:x") == 0);
  TASK_CHECK(trace.count("create:y") == 1);

  b.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  TASK_CHECK(trace.count("destroy:y") == 0);

  c.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:y") == 1);
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("create:y") == 1);
  return 0;
}

static int lifetimeFutureUseKeepsAlive() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate y{&trace, "Y"};
  Gate b{&trace, "B"};
  FrameGraph fg;
  FrameGraphResource x{};
  FrameGraphResource yId{};
  CpuTexture *texture{nullptr};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      x = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(x);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      texture = &resources.get<CpuTexture>(data.texture);
      texture->value = 42;
      trace.add("submit:A");
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "Y",
    [&](FrameGraph::Builder &builder, PassData &data) {
      yId = builder.create<CpuTexture>("y", {&trace, "y"});
      data.texture = builder.write(yId);
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:Y");
      return y.handle();
    });
  // The future consumer is blocked by both A and Y, so it stays unsubmitted
  // while A finishes.
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      builder.read(x);
      data.texture = builder.read(yId);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &t = resources.get<CpuTexture>(data.texture);
      (void)t;
      trace.add("submit:B");
      return b.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("submit:Y") == 1);

  // A finishes out of order, before B has even been submitted. x must stay
  // alive because a future use is still pending.
  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 0);
  TASK_CHECK(texture != nullptr);
  TASK_CHECK(texture->alive);
  TASK_CHECK(trace.count("submit:B") == 0);

  y.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:B") == 1);
  // The reader still sees the value produced by A, which finished earlier.
  TASK_CHECK(texture->value == 42);
  TASK_CHECK(trace.count("destroy:x") == 0);

  b.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("destroy:x") == 1);
  TASK_CHECK(trace.count("destroy:y") == 1);
  return 0;
}

static int lifetimeCulledPass() {
  Trace trace;
  Gate live{&trace, "live"};
  Gate culled{&trace, "culled"};
  FrameGraph fg;

  fg.addAsyncCallbackPass<PassData>(
    "live",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:live");
      trace.add("preRead:check:0"); // Never emitted by the culled pass.
      return live.handle();
    });
  // No side effect and its output is not consumed: culled.
  fg.addAsyncCallbackPass<PassData>(
    "culled",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("z", {&trace, "z"});
      data.texture = builder.write(data.texture, 22);
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      (void)resources.get<CpuTexture>(data.texture);
      trace.add("submit:culled");
      return culled.handle();
    });
  fg.compile();
  fg.startExecution();

  live.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(trace.count("submit:culled") == 0);
  TASK_CHECK(culled.queries == 0);
  TASK_CHECK(trace.count("create:z") == 0);
  TASK_CHECK(trace.count("destroy:z") == 0);
  TASK_CHECK(trace.count("preWrite:z:22") == 0);
  return 0;
}

static int lifetimeImportedAndHooks() {
  Trace trace;
  Gate a{&trace, "A"};
  Gate b{&trace, "B"};
  FrameGraph fg;
  CpuTexture back;
  back.alive = true;
  back.value = 0;
  const auto x =
    fg.import("x", CpuTexture::Desc{&trace, "x"}, std::move(back));
  FrameGraphResource written{};
  int observed{-1};

  // Writing an imported resource counts as a side effect.
  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.write(x, 22);
      written = data.texture;
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      texture.value = 9;
      trace.add("submit:A");
      return a.handle();
    });
  fg.addAsyncCallbackPass<PassData>(
    "B",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.read(written, 11);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      observed = resources.get<CpuTexture>(data.texture).value;
      trace.add("submit:B");
      return b.handle();
    });
  fg.compile();
  fg.startExecution();

  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("preWrite:x:22") == 1);
  a.release();
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("preRead:x:11") == 1);
  b.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(observed == 9);
  TASK_CHECK(trace.count("create:x") == 0);
  TASK_CHECK(trace.count("destroy:x") == 0);
  return 0;
}

static int lifetimeSecondSession() {
  Trace trace;
  Gate gate{&trace, "A"};
  FrameGraph fg;
  int runs{0};

  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &data, FrameGraphPassResources &resources, void *) {
      auto &texture = resources.get<CpuTexture>(data.texture);
      texture.value = ++runs;
      trace.add("submit:A");
      return gate.handle();
    });
  fg.compile();

  // -- session 1
  fg.startExecution();
  TASK_CHECK(!fg.pollExecution());
  gate.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(runs == 1);
  TASK_CHECK(trace.count("create:x") == 1);
  TASK_CHECK(trace.count("destroy:x") == 1);

  // Restarting before the previous session finished would be a logic error.
  // (Already finished here, so it must succeed.)

  // -- session 2, all completion and allocation state is reset
  gate.rearm();
  fg.startExecution();
  TASK_CHECK(trace.count("submit:A") == 1); // Nothing submitted yet.
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 2);
  TASK_CHECK(trace.count("create:x") == 2);
  gate.release();
  TASK_CHECK(fg.pollExecution());
  TASK_CHECK(runs == 2);
  TASK_CHECK(trace.count("destroy:x") == 2);
  return 0;
}

static int lifetimeRestartWhileActiveRejected() {
  Trace trace;
  Gate gate{&trace, "A"};
  FrameGraph fg;
  fg.addAsyncCallbackPass<PassData>(
    "A",
    [&](FrameGraph::Builder &builder, PassData &data) {
      data.texture = builder.create<CpuTexture>("x", {&trace, "x"});
      data.texture = builder.write(data.texture);
      builder.setSideEffect();
    },
    [&](const PassData &, FrameGraphPassResources &, void *) {
      trace.add("submit:A");
      return gate.handle();
    });
  fg.compile();
  fg.startExecution();

  // start twice: rejected before a second submission happens.
  TASK_CHECK(throwsLogicError([&] { fg.startExecution(); }));
  TASK_CHECK(trace.count("submit:A") == 0);
  TASK_CHECK(!fg.pollExecution());
  TASK_CHECK(trace.count("submit:A") == 1);

  // execute() during a session is rejected too, with no side effect.
  TASK_CHECK(throwsLogicError([&] { fg.execute(); }));
  TASK_CHECK(trace.count("submit:A") == 1);
  TASK_CHECK(trace.count("create:x") == 1);

  gate.release();
  TASK_CHECK(fg.pollExecution());
  return 0;
}

//

const task_case task_cases[] = {
  {"readiness", "start_defers_submission", readinessStartDefersSubmission},
  {"readiness", "blocked_later_independent", readinessBlockedLaterIndependent},
  {"readiness", "immediate_unlock", readinessImmediateUnlock},
  {"readiness", "mixed_sync_and_async", readinessMixedSyncAndAsync},
  {"readiness", "single_query_per_poll", readinessSingleQueryPerPoll},
  {"readiness", "rejects_misuse", readinessRejectsMisuse},

  {"hazards", "write_after_readers", hazardsWriteAfterReaders},
  {"hazards", "read_after_write", hazardsReadAfterWrite},
  {"hazards", "renamed_handle_shares_object", hazardsRenamedHandleSharesObject},
  {"hazards", "independent_resources", hazardsIndependentResources},
  {"hazards", "out_of_order_completion", hazardsOutOfOrderCompletion},
  {"hazards", "read_modify_write_chain", hazardsReadModifyWriteChain},
  {"hazards", "create_dependency", hazardsCreateDependency},

  {"lifetime", "create_destroy_once", lifetimeCreateDestroyOnce},
  {"lifetime", "future_use_keeps_alive", lifetimeFutureUseKeepsAlive},
  {"lifetime", "culled_pass", lifetimeCulledPass},
  {"lifetime", "imported_and_hooks", lifetimeImportedAndHooks},
  {"lifetime", "second_session", lifetimeSecondSession},
  {"lifetime", "restart_while_active_rejected", lifetimeRestartWhileActiveRejected},
};
const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
