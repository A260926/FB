// example.standalone.wren
//
// A directly runnable instance of the open-upvalue plan sub-language. It can
// be executed by any plain Wren host (no embedding handshake is required) and
// prints the reference values derived by the C model in upvalue_model.c.
//
// It contains, in one program:
//   * a read closure and a write closure sharing one mutable cell;
//   * a suspended Fiber whose local is an open capture while it yields;
//   * two levels of indirect capture through intermediate functions;
//   * a capture that is closed after its declaring block exits.

var results = List.new()

// --- shared cell: one reader and one writer on the same local ---------------
var cell = 11
var reader = Fn.new { cell }
var writer = Fn.new { cell = 99 }

results.add(reader.call())   // expect: 11
results.add(writer.call())   // expect: 99 (assignment block yields the value)
results.add(reader.call())   // expect: 99 (the alias to the cell is intact)

// --- two levels of indirect capture ----------------------------------------
// Each intermediate function only forwards; the compiler threads the same
// cell through both layers as a flattened chain of upvalues.
var indirect = Fn.new { Fn.new { Fn.new { cell }.call() }.call() }
results.add(indirect.call()) // expect: 99

// --- a Fiber holding an open capture while suspended ------------------------
var fiber = Fiber.new {
  var local = 105
  var get = Fn.new { local }
  var set = Fn.new { local = 77 }

  Fiber.yield(get.call())    // resume 1: open capture, value 105
  // The resume that follows happens after the host side has run; the cell is
  // still open and the alias between get and local must survive.
  set.call()
  return get.call()          // final resume: shared cell now 77
}

results.add(fiber.call())    // expect: 105
results.add(fiber.call())    // expect: 77

// --- a capture closed after the declaring block exits ----------------------
var kept = null
{
  var inner = 31
  var innerGet = Fn.new { inner }
  kept = innerGet
  // On this block exit the open upvalue for "inner" is closed into the
  // closure; the stack slot can be reused but the captured value must live.
}
results.add(kept.call())     // expect: 31

for (value in results) System.print(value)
// expect: 11
// expect: 99
// expect: 99
// expect: 99
// expect: 105
// expect: 77
// expect: 31
