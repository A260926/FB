// Model driven private name syntax regression tests.
//
// The generated programs are only ever handed to Parser.parse; they are
// never executed, and expectations come from PrivateScopeModel, not from
// eval/Function/vm or any host engine compilation.

import assert from "node:assert/strict"
import {readFile} from "node:fs/promises"
import {test} from "node:test"
import {dirname, join, resolve} from "node:path"
import {fileURLToPath} from "node:url"
import {
  PARSE_OPTIONS,
  PARSER_PATCHES,
  PrivateScopeModel,
  Rng,
  cloneModel,
  defaultCliArgs,
  formatCounts,
  loadBaselineParser,
  loadPatchedParser,
  renderProgram,
  runFuzz,
  transformReorder,
  verifyAccepted,
  verifyAgainstModel,
  verifyRejected,
  writeDivergenceArtifacts
} from "./private-scope-model.mjs"

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..")
const STATEMENT_SOURCE = join(REPO_ROOT, "acorn/src/statement.js")

assert.equal(PARSE_OPTIONS.ecmaVersion, 2022)
assert.equal(PARSE_OPTIONS.checkPrivateFields, true)

const loaded = await loadBaselineParser()
const parser = loaded.parser
const originalStatementSource = await readFile(STATEMENT_SOURCE, "utf8")

test.after(async () => {
  await loaded.dispose()
})

// Locate the nth occurrence of a token in a hand written source string so
// that readable fixtures can still carry exact UTF-16 marker ranges.
function rangeOf(source, token, occurrence = 0) {
  let index = -1
  for (let i = 0; i <= occurrence; ++i) {
    index = source.indexOf(token, index + 1)
    assert.notEqual(index, -1, `token ${JSON.stringify(token)} #${i} not found`)
  }
  return {start: index, end: index + token.length}
}

function rangesFor(source, specs) {
  const ranges = new Map()
  for (const [id, token, occurrence] of specs) ranges.set(id, rangeOf(source, token, occurrence))
  return ranges
}

function classModel(className) {
  const model = new PrivateScopeModel(0)
  const env = model.addClass(null, 0)
  env.className = className
  return {model, env}
}

function nestedModel(outerName, innerName) {
  const model = new PrivateScopeModel(0)
  const outer = model.addClass(null, 0)
  outer.className = outerName
  const inner = model.addClass(outer, 1)
  inner.className = innerName
  return {model, outer, inner}
}

test("forward", () => {
  // Use sites may refer to declarations that appear later in the same body.
  {
    const source = "class A{ m(o){ return o.#x; } #x; }"
    const {model, env} = classModel("A")
    const use = env.addUse("x", "body", "member", "method")
    const decl = env.addDeclaration("x", "field", false)
    const ranges = rangesFor(source, [[use.id, "#x", 0], [decl.id, "#x", 1]])
    assert.equal(model.accepted, true)
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // Forward reference from a field initializer.
  {
    const source = "class A{ a = o.#x; #x; }"
    const {model, env} = classModel("A")
    const use = env.addUse("x", "body", "member", "field")
    const decl = env.addDeclaration("x", "field", false)
    const ranges = rangesFor(source, [[use.id, "#x", 0], [decl.id, "#x", 1]])
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // Forward reference through a `#x in o` test.
  {
    const source = "class A{ m(o){ return #x in o; } #x; }"
    const {model, env} = classModel("A")
    const use = env.addUse("x", "body", "in", "method")
    const decl = env.addDeclaration("x", "field", false)
    const ranges = rangesFor(source, [[use.id, "#x", 0], [decl.id, "#x", 1]])
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // A name that is never declared is rejected at the use site.
  {
    const source = "class A{ m(o){ return o.#missing; } }"
    const {model, env} = classModel("A")
    const use = env.addUse("missing", "body", "member", "method")
    const ranges = rangesFor(source, [[use.id, "#missing", 0]])
    assert.equal(model.accepted, false)
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.markerId, use.id)
  }
  // Moving declarations in front of or behind the uses must not change
  // legality: the model ignores declaration order.
  {
    const {model, env} = classModel("A")
    env.addDeclaration("p", "field", false)
    env.addDeclaration("q", "method", false)
    env.addUse("p", "body", "member", "method")
    env.addUse("q", "body", "in", "method")
    const first = renderProgram(model)
    assert.equal(verifyAccepted(parser, model, first.source, first.ranges).ok, true)
    const reordered = transformReorder(model, new Rng(7))
    assert.equal(reordered.model.accepted, true)
    assert.equal(verifyAccepted(parser, reordered.model, reordered.source, reordered.ranges).ok, true)
    assert.notEqual(first.source, reordered.source)
  }
})

test("nested", () => {
  // Inner class reaches a private name declared by the enclosing class.
  {
    const source = "class Outer{ #outer; m(){ return class Inner{ n(o){ return o.#outer; } }; } }"
    const {model, outer, inner} = nestedModel("Outer", "Inner")
    const decl = outer.addDeclaration("outer", "field", false)
    const use = inner.addUse("outer", "body", "member", "method")
    const ranges = rangesFor(source, [[decl.id, "#outer", 0], [use.id, "#outer", 1]])
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // Arrows, ordinary functions and static blocks do not open private
  // environments; all of these resolve against the surrounding class.
  {
    const source = "class A{ #x; m(){ const f=(o)=>o.#x; const g=function(o){return o.#x;}; return f; } static { o.#x; } }"
    const {model, env} = classModel("A")
    const decl = env.addDeclaration("x", "field", false)
    const arrow = env.addUse("x", "body", "member", "arrow")
    const fn = env.addUse("x", "body", "member", "function")
    const block = env.addUse("x", "body", "member", "static")
    const ranges = rangesFor(source, [
      [decl.id, "#x", 0], [arrow.id, "#x", 1], [fn.id, "#x", 2], [block.id, "#x", 3]
    ])
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // A nested class declaration shadows the outer name of the same spelling.
  {
    const source = "class Outer{ #x; m(){ return class Inner{ #x; n(o){ return o.#x; } }; } }"
    const {model, outer, inner} = nestedModel("Outer", "Inner")
    const outerDecl = outer.addDeclaration("x", "field", false)
    const innerDecl = inner.addDeclaration("x", "field", false)
    const use = inner.addUse("x", "body", "member", "method")
    const ranges = rangesFor(source, [
      [outerDecl.id, "#x", 0], [innerDecl.id, "#x", 1], [use.id, "#x", 2]
    ])
    assert.equal(model.resolve(inner, "body", "x"), inner)
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // An inner use that no enclosing class declares is rejected.
  {
    const source = "class Outer{ #x; m(){ return class Inner{ n(o){ return o.#y; } }; } }"
    const {model, outer, inner} = nestedModel("Outer", "Inner")
    outer.addDeclaration("x", "field", false)
    const use = inner.addUse("y", "body", "member", "method")
    const ranges = rangesFor(source, [[use.id, "#y", 0]])
    assert.equal(model.accepted, false)
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.markerId, use.id)
  }
  // Adding the missing name to the outer class repairs the use.
  {
    const broken = new PrivateScopeModel(0)
    const outer = broken.addClass(null, 0)
    const inner = broken.addClass(outer, 1)
    outer.addDeclaration("x", "field", false)
    inner.addUse("y", "body", "member", "method")
    assert.equal(broken.accepted, false)
    const repaired = cloneModel(broken)
    repaired.classes[0].addDeclaration("y", "field", false)
    assert.equal(repaired.accepted, true)
    const rendered = renderProgram(repaired)
    assert.equal(verifyAccepted(parser, repaired, rendered.source, rendered.ranges).ok, true)
  }
})

test("heritage", () => {
  // The extends expression resolves through enclosing class environments.
  {
    const source = "class Outer{ #x; m(o){ return class Inner extends (o.#x){} } }"
    const {model, outer, inner} = nestedModel("Outer", "Inner")
    const decl = outer.addDeclaration("x", "field", false)
    const use = inner.addUse("x", "heritage", "member", "heritage")
    const ranges = rangesFor(source, [[decl.id, "#x", 0], [use.id, "#x", 1]])
    assert.equal(model.resolve(inner, "heritage", "x"), outer)
    assert.equal(verifyAgainstModel(parser, model, source, ranges).ok, true)
  }
  // A name declared only in the class's own body cannot rescue its heritage
  // expression: the body environment does not exist yet while extends runs.
  {
    const source = "class Outer{ m(o){ return class Inner extends (o.#x){ #x; } } }"
    const {model, inner} = nestedModel("Outer", "Inner")
    const heritageUse = inner.addUse("x", "heritage", "member", "heritage")
    const bodyDecl = inner.addDeclaration("x", "field", false)
    const ranges = rangesFor(source, [[heritageUse.id, "#x", 0], [bodyDecl.id, "#x", 1]])
    assert.equal(model.accepted, false)
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.markerId, heritageUse.id)
  }
  // Same boundary for the `#x in o` heritage form.
  {
    const source = "class Outer{ m(o){ return class Inner extends (#x in o){ #x; } } }"
    const {model, inner} = nestedModel("Outer", "Inner")
    const heritageUse = inner.addUse("x", "heritage", "in", "heritage")
    const bodyDecl = inner.addDeclaration("x", "field", false)
    const ranges = rangesFor(source, [[heritageUse.id, "#x", 0], [bodyDecl.id, "#x", 1]])
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.kind, "undeclared")
  }
  // Repair matrix: adding the name to the outer class fixes heritage use;
  // adding it only to the class's own body does not.
  {
    const broken = new PrivateScopeModel(0)
    const outer = broken.addClass(null, 0)
    const inner = broken.addClass(outer, 1)
    inner.addUse("x", "heritage", "member", "heritage")
    inner.addDeclaration("x", "field", false)
    assert.equal(broken.expectedError().kind, "undeclared")

    const stillBroken = renderProgram(broken)
    assert.equal(verifyRejected(parser, broken, stillBroken.source, stillBroken.ranges).ok, true)

    const repaired = cloneModel(broken)
    repaired.classes[0].addDeclaration("x", "field", false)
    assert.equal(repaired.accepted, true)
    const rendered = renderProgram(repaired)
    assert.equal(verifyAccepted(parser, repaired, rendered.source, rendered.ranges).ok, true)
  }
})

test("accessors", () => {
  const pairCase = (source, staticPair) => {
    const {model, env} = classModel("A")
    const first = env.addDeclaration("x", "get", staticPair)
    const second = env.addDeclaration("x", "set", staticPair)
    const ranges = rangesFor(source, [[first.id, "#x", 0], [second.id, "#x", 1]])
    assert.equal(model.accepted, true)
    assert.equal(verifyAccepted(parser, model, source, ranges).ok, true)
  }

  // Legal instance and static get/set pairs.
  pairCase("class A{ get #x(){} set #x(v){} }", false)
  pairCase("class A{ static get #x(){} static set #x(v){} }", true)
  // Static/instance crossing is a conflict, reported at the second member.
  {
    const source = "class A{ get #x(){} static set #x(v){} }"
    const {model, env} = classModel("A")
    const first = env.addDeclaration("x", "get", false)
    const second = env.addDeclaration("x", "set", true)
    const ranges = rangesFor(source, [[first.id, "#x", 0], [second.id, "#x", 1]])
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.markerId, second.id)
  }
  {
    const source = "class A{ static get #x(){} set #x(v){} }"
    const {model, env} = classModel("A")
    const first = env.addDeclaration("x", "get", true)
    const second = env.addDeclaration("x", "set", false)
    const ranges = rangesFor(source, [[first.id, "#x", 0], [second.id, "#x", 1]])
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.markerId, second.id)
  }

  // Two getters cannot pair.
  {
    const source = "class A{ get #x(){} get #x(){} }"
    const {model, env} = classModel("A")
    const first = env.addDeclaration("x", "get", false)
    const second = env.addDeclaration("x", "get", false)
    const ranges = rangesFor(source, [[first.id, "#x", 0], [second.id, "#x", 1]])
    assert.equal(verifyRejected(parser, model, source, ranges).ok, true)
    assert.equal(model.expectedError().markerId, second.id)
  }
  // Fields mixed with accessors conflict.
  {
    const source = "class A{ #x; get #x(){} }"
    const {model, env} = classModel("A")
    const field = env.addDeclaration("x", "field", false)
    const getter = env.addDeclaration("x", "get", false)
    const ranges = rangesFor(source, [[field.id, "#x", 0], [getter.id, "#x", 1]])
    assert.equal(verifyRejected(parser, model, source, ranges).ok, true)
  }
  // Instance/static field crossing conflicts.
  {
    const source = "class A{ #x; static #x; }"
    const {model, env} = classModel("A")
    const instance = env.addDeclaration("x", "field", false)
    const staticField = env.addDeclaration("x", "field", true)
    const ranges = rangesFor(source, [[instance.id, "#x", 0], [staticField.id, "#x", 1]])
    const result = verifyRejected(parser, model, source, ranges)
    assert.equal(result.ok, true)
    assert.equal(result.expectedError.markerId, staticField.id)
  }
  // Reordering the two halves of a legal pair keeps it legal.
  {
    const {model, env} = classModel("A")
    env.addDeclaration("x", "get", true)
    env.addDeclaration("x", "set", true)
    const reordered = transformReorder(model, new Rng(11))
    const rendered = renderProgram(reordered.model)
    assert.equal(verifyAccepted(parser, reordered.model, rendered.source, rendered.ranges).ok, true)
  }
})

test("model", async () => {
  assert.deepEqual(defaultCliArgs(["--seed", "1320", "--cases", "160"]), {seed: 1320, cases: 160})
  const counts = await runFuzz(parser, {seed: 1320, cases: 160})
  console.log("\n" + formatCounts(counts))

  // Both verdicts and every generated shape must actually occur.
  assert.ok(counts.accepted > 0, "no accepted programs generated")
  assert.ok(counts.rejected > 0, "no single-fault rejected programs generated")
  for (const type of Object.keys(counts.mutants)) {
    assert.ok(counts.mutants[type] > 0, `mutant type ${type} never generated`)
  }
  for (const type of Object.keys(counts.transforms)) {
    assert.ok(counts.transforms[type] > 0, `transform ${type} never applied`)
  }
  assert.ok(counts.heritage > 0, "no heritage uses generated")
  assert.ok(counts.shadow > 0, "no shadowing declarations generated")
  assert.ok(counts.accessorPairs > 0, "no accessor pairs generated")
  assert.equal(counts.unicodeFixture, 1)

  if (counts.divergences.length) {
    const files = await writeDivergenceArtifacts(counts)
    assert.fail(`model/parser divergences: ${counts.divergences.length}; ` +
      `source, model, seed and first error range saved to ${files.join(", ")}`)
  }
})

test("mutants", async () => {
  // Each patch lives in a separate temporary source copy. The working tree
  // parser sources must remain byte-for-byte unchanged.
  for (const patch of PARSER_PATCHES) {
    const patched = await loadPatchedParser(patch)
    try {
      // The patched module must load and its Parser.parse must actually run
      // on a benign program before any detection claim is made.
      const smoke = patched.parser.parse("class Smoke{}", PARSE_OPTIONS)
      assert.equal(smoke.type, "Program")

      if (patch.name === "outer-private-env-lookup") {
        const source = "class Outer{ #outer; m(){ return class Inner{ n(o){ return o.#outer; } }; } }"
        const model = new PrivateScopeModel(0)
        const outer = model.addClass(null, 0)
        const inner = model.addClass(outer, 1)
        const decl = outer.addDeclaration("outer", "field", false)
        const use = inner.addUse("outer", "body", "member", "method")
        const ranges = rangesFor(source, [[decl.id, "#outer", 0], [use.id, "#outer", 1]])
        assert.equal(model.accepted, true)
        assert.equal(verifyAccepted(parser, model, source, ranges).ok, true)

        let thrown
        try {
          patched.parser.parse(source, PARSE_OPTIONS)
        } catch (error) {
          thrown = error
        }
        assert.ok(thrown instanceof SyntaxError, `${patch.name} did not change the verdict`)
        const range = ranges.get(use.id)
        assert.ok(thrown.pos >= range.start && thrown.pos < range.end,
          `${patch.name} error pos ${thrown.pos} misses the outer-lookup use marker`)
      }

      if (patch.name === "accessor-static-pairing") {
        const source = "class A{ static get #x(){} static set #x(v){} }"
        const model = new PrivateScopeModel(0)
        const env = model.addClass(null, 0)
        const getter = env.addDeclaration("x", "get", true)
        const setter = env.addDeclaration("x", "set", true)
        const ranges = rangesFor(source, [[getter.id, "#x", 0], [setter.id, "#x", 1]])
        assert.equal(verifyAccepted(parser, model, source, ranges).ok, true)

        let thrown
        try {
          patched.parser.parse(source, PARSE_OPTIONS)
        } catch (error) {
          thrown = error
        }
        assert.ok(thrown instanceof SyntaxError, `${patch.name} did not change the verdict`)
        const range = ranges.get(setter.id)
        assert.ok(thrown.pos >= range.start && thrown.pos < range.end,
          `${patch.name} error pos ${thrown.pos} misses the second accessor marker`)
      }

      if (patch.name === "heritage-class-body-boundary") {
        const source = "class Outer{ m(o){ return class Inner extends (o.#x){ #x; } } }"
        const model = new PrivateScopeModel(0)
        const outer = model.addClass(null, 0)
        const inner = model.addClass(outer, 1)
        const heritageUse = inner.addUse("x", "heritage", "member", "heritage")
        const bodyDecl = inner.addDeclaration("x", "field", false)
        const ranges = rangesFor(source, [[heritageUse.id, "#x", 0], [bodyDecl.id, "#x", 1]])

        const baseline = verifyRejected(parser, model, source, ranges)
        assert.equal(baseline.ok, true)
        assert.equal(baseline.expectedError.markerId, heritageUse.id)

        // The boundary break lets the heritage clause borrow a name that is
        // only declared in the class body, flipping rejection to acceptance.
        const result = verifyRejected(patched.parser, model, source, ranges)
        assert.equal(result.ok, false)
        assert.match(result.reason, /accepted a program the model rejects/)
      }
    } finally {
      await patched.dispose()
    }
  }

  const after = await readFile(STATEMENT_SOURCE, "utf8")
  assert.equal(after, originalStatementSource, "acorn/src/statement.js was modified in place")
})
