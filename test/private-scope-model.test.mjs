// Model-driven private-name syntax regression for acorn.
//
// The first four tests hold directly readable positive/negative cases.
// `model` runs the seeded generator and transform families. `mutants`
// patches temporary copies of the parser source and requires the model
// to catch each behavioral difference.

import test from "node:test"
import assert from "node:assert/strict"
import {readFileSync, writeFileSync} from "node:fs"
import {join} from "node:path"
import {loadParser, makeParserCopy, importParser, parserSourceDir} from "../baseline/load-parser.mjs"
import {
  checkCase,
  runBatch,
  saveFailure,
  scanPrivateNames,
  buildMutantDetectionCases,
  CATEGORIES,
  REJECT_CATEGORIES
} from "./private-scope-model.mjs"

const {parse} = await loadParser()

// Builds a fixed case from a readable code string. Expected private
// names and UTF-16 ranges are derived from the source text itself,
// never from the parser under test.
function fixedAccept(code) {
  return {code, expect: "accept", names: scanPrivateNames(code), errorName: null}
}

// `errorIndex` selects which scanned private-name occurrence the
// controlled error must point at.
function fixedReject(code, errorIndex) {
  const names = scanPrivateNames(code)
  return {code, expect: "reject", names, errorName: names[errorIndex]}
}

function assertCases(cases) {
  for (const kase of cases) {
    const res = checkCase(parse, kase)
    assert.ok(res.ok, `${kase.expect} case failed: ${kase.code}\n  ${res.detail}`)
  }
}

test("forward", () => {
  assertCases([
    // A use may precede its declaration inside a class body.
    fixedAccept("class C { m() { return this.#x } #x = 1 }"),
    fixedAccept("class C { static { obj.#x } static #x = 2 }"),
    fixedAccept("class C { n() { return this.#x } #x = 1; m() { return this.#x } }"),
    // Moving the declaration before or after the use changes nothing.
    fixedAccept("class C { #x = 1; m() { return this.#x } }"),
    fixedAccept("class C { m() { return this.#x } #x = 1 }"),
    // Fixed boundary: unicode escapes are legal in private names and
    // cook to the same identifier (#a is #a).
    fixedAccept("class C { m() { return this.#a } #\\u0061 = 1 }"),
    fixedAccept("class C { m() { return this.#\\u0061 } #a = 1 }"),
    // A use of a name that is never declared is rejected at the use.
    fixedReject("class C { m() { return this.#y } #x = 1 }", 0),
    // #b is not the same name as the escaped declaration #a (#a).
    fixedReject("class C { m() { return this.#b } #\\u0061 = 1 }", 0)
  ])
})

test("nested", () => {
  assertCases([
    // A nested class reaches out to the outer class's declaration.
    fixedAccept("class Outer { #x = 1; Inner = class { m() { return this.#x } } }"),
    // The inner declaration shadows the outer one of the same name.
    fixedAccept("class Outer { #x = 1; Inner = class { #x = 2; m() { return this.#x } } }"),
    // Functions, arrows and methods do not create private-name scopes.
    fixedAccept("class C { #x = 1; m() { function f() { return () => obj.#x } return f() } }"),
    // A use three levels down binds to the outermost declaration.
    fixedAccept("class A { #x = 1; B = class { C = class { m() { return this.#x } } } }"),
    // A plain property and a string literal are not private uses.
    fixedAccept("class C { #x = 1; m() { return obj.x + this.#x } }"),
    fixedAccept("class C { m() { return obj['#x'] } }"),
    // Undeclared in the inner class, undeclared everywhere: rejected at
    // the use, however deep it sits.
    fixedReject("class Outer { Inner = class { m() { return this.#x } } }", 0),
    fixedReject("class A { B = class { C = class { m() { return this.#u } } } }", 0)
  ])
})

test("heritage", () => {
  assertCases([
    // The extends expression belongs to the environment in effect
    // before the class body: it sees the outer declaration.
    fixedAccept("class Outer { #x = 1; Inner = class extends (base.#x) {} }"),
    fixedAccept("class Outer { #x = 1; Inner = class extends (base.#x) { #x = 2 } }"),
    // A class nested inside a heritage expression keeps its own scope.
    fixedAccept("class A extends (class { #y = 1; m() { return this.#y } }) {}"),
    // A heritage use inside a method's class expression binds outward.
    fixedAccept("class C { #g = 1; m() { return class extends (base.#g) {} } }"),
    // The class's own body declaration cannot fix its extends use.
    fixedReject("class Outer { Inner = class extends (base.#x) { #x = 1 } }", 0),
    // ...but declaring the name on the outer class does fix it.
    fixedAccept("class Outer { #x = 1; Inner = class extends (base.#x) { #x = 1 } }"),
    // At the top level there is no enclosing class at all.
    fixedReject("class A extends (base.#z) { #z = 1 }", 0)
  ])
})

test("accessors", () => {
  assertCases([
    // A getter pairs with a setter of the same name and static-ness.
    fixedAccept("class C { get #p() { return 1 } set #p(v) {} }"),
    fixedAccept("class C { set #p(v) {} get #p() { return 1 } }"),
    fixedAccept("class C { static get #p() { return 1 } static set #p(v) {} }"),
    // Uses may reference the paired name before both declarations.
    fixedAccept("class C { m() { return this.#p } get #p() { return 1 } set #p(v) {} }"),
    // Two getters clash (rejected at the second declaration's name).
    fixedReject("class C { get #p() { return 1 } get #p() { return 2 } }", 1),
    // Static and instance accessors of the same name do not pair.
    fixedReject("class C { static get #p() { return 1 } set #p(v) {} }", 1),
    fixedReject("class C { get #p() { return 1 } static set #p(v) {} }", 1),
    // A field clashes with a same-name accessor.
    fixedReject("class C { #p = 1; get #p() { return 1 } }", 1),
    // A completed pair cannot be extended by a third accessor.
    fixedReject("class C { get #p() { return 1 } set #p(v) {} set #p(v) {} }", 2)
  ])
})

test("model", () => {
  const seed = 1320
  const cases = 160
  let saved = null
  const {counts, failures} = runBatch({
    seed,
    cases,
    parse,
    onFailure(failure) { saved = saveFailure(failure) }
  })
  const summary = Object.keys(counts).sort()
    .filter(key => counts[key] > 0)
    .map(key => `${key}=${counts[key]}`)
  console.log(`private-scope-model seed=${seed} cases=${cases}`)
  for (const line of summary) console.log(`  ${line}`)
  if (failures.length) {
    const failure = failures[0]
    assert.fail(
      `model/parser divergence at case ${failure.caseIndex} ` +
      `(${failure.kase.category}, transform ${failure.kase.transform}): ${failure.result.detail}\n` +
      `code: ${failure.kase.code}\nsaved to ${saved}`
    )
  }
  // Every category of normal program and of single controlled error
  // must actually appear.
  for (const category of CATEGORIES) {
    const verdict = REJECT_CATEGORIES.has(category) ? "reject" : "accept"
    assert.ok(counts[`case:${verdict}:${category}`] > 0, `category ${category} (${verdict}) did not appear`)
  }
})

// Three small, reviewable patches against acorn/src/statement.js. Each
// is applied to a temporary copy only; the original sources are never
// modified.
const MUTANTS = [
  {
    name: "outer-lookup",
    description: "undeclared uses are no longer propagated to the outer class environment",
    find: "parent.used.push(id)",
    replace: "/* mutant: outer private-name lookup disabled */",
    kase: "outerLookup"
  },
  {
    name: "accessor-static-pairing",
    description: "getter/setter pairing ignores the static modifier",
    find: "(element.static ? \"s\" : \"i\") + element.kind",
    replace: "\"i\" + element.kind",
    kase: "accessorStatic"
  },
  {
    name: "extends-env-boundary",
    description: "the class body environment is entered before the extends expression",
    find: "  this.parseClassSuper(node)\n  const privateNameMap = this.enterClassBody()",
    replace: "  const privateNameMap = this.enterClassBody()\n  this.parseClassSuper(node)",
    kase: "extendsBoundary"
  }
]

test("mutants", async () => {
  const detection = buildMutantDetectionCases()
  // The unmodified parser must agree with the model on every case;
  // otherwise a detection below would prove nothing.
  for (const kase of Object.values(detection)) {
    const res = checkCase(parse, kase)
    assert.ok(res.ok, `original parser diverges on ${kase.category}: ${res.detail}`)
  }
  for (const mutant of MUTANTS) {
    const dir = makeParserCopy("acorn-mutant-")
    const file = join(dir, "statement.js")
    const source = readFileSync(file, "utf8")
    assert.ok(source.includes(mutant.find), `mutant ${mutant.name}: patch target not found`)
    assert.equal(source.split(mutant.find).length, 2, `mutant ${mutant.name}: patch target not unique`)
    writeFileSync(file, source.replace(mutant.find, mutant.replace))
    // The patched copy must load and start on its own; a module load
    // failure or a parser that cannot start would not count as a
    // detection.
    const acorn = await importParser(dir)
    assert.equal(typeof acorn.parse, "function", `mutant ${mutant.name}: copy failed to load`)
    let calls = 0
    const mutantParse = (code, options) => {
      calls++
      return acorn.parse(code, options)
    }
    // The mutant parser still parses sane programs correctly.
    for (const sane of [detection.sanity, detection.sanityHeritage]) {
      const res = checkCase(mutantParse, sane)
      assert.ok(res.ok, `mutant ${mutant.name} broke basic parsing: ${res.detail}`)
    }
    // The same model catches the behavioral difference, as a concrete
    // accept/reject or name-position difference.
    const res = checkCase(mutantParse, detection[mutant.kase])
    assert.ok(!res.ok, `mutant ${mutant.name} (${mutant.description}) was not detected`)
    assert.ok(
      res.kind === "accept/reject" || res.kind === "name-position",
      `mutant ${mutant.name} detected with unexpected kind ${res.kind}: ${res.detail}`
    )
    assert.ok(calls >= 3, `mutant ${mutant.name}: parse was not actually called`)
    console.log(`mutant ${mutant.name} detected: ${res.kind}: ${res.detail}`)
  }
  // The original sources must be untouched by all of the above.
  const original = readFileSync(join(parserSourceDir, "statement.js"), "utf8")
  for (const mutant of MUTANTS) assert.ok(original.includes(mutant.find), `original source was modified by mutant ${mutant.name}`)
})
