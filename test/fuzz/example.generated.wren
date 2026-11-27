var log = List.new()
var parkedFn = null
var parkedFiber = null
class Deep {
  static run(n) {
    if (n == 0) return 0
    return Deep.run(n - 1)
  }
}
var Driver = Fiber.new {
  var v0 = 11
  var v1 = Fn.new { v0 }
  var v2 = Fn.new { v0 = 99 }
  var v3 = Fn.new { Fn.new { Fn.new { v0 }.call() }.call() }
  var v4 = 0
  parkedFn = v1
  Fiber.yield(0)
  Fiber.yield(0)
  parkedFn = null
  Fiber.yield(0)
  log.add(v2.call())
  log.add(v1.call())
  log.add(v3.call())
  var v8 = Fiber.new {
    var v5 = 105
    var v6 = Fn.new { v5 }
    var v7 = Fn.new { v5 = 77 }
    Fiber.yield(v6.call())
    Deep.run(26)
    Fiber.yield(v6.call())
    v7.call()
    return v6.call()
  }
  parkedFiber = v8
  Fiber.yield(0)
  Fiber.yield(0)
  parkedFiber = null
  Fiber.yield(0)
  Deep.run(26)
  log.add(v8.call())
  {
    var v9 = 31
    var v10 = Fn.new { v9 }
    v4 = v10
    log.add(Object.same(v4, v10))
  }
  Fiber.yield(0)
  log.add(v4.call())
}
