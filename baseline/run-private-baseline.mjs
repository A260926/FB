// Re-runs six existing private-name parse checks from the repository's
// own test suite (test/tests-class-features-2022.js) against a freshly
// loaded copy of acorn/src. This is the baseline: the checks and their
// expected messages/positions are the ones already shipped in the repo.

import {createRequire} from "node:module"
import {loadParser} from "./load-parser.mjs"

const require = createRequire(import.meta.url)
const driver = require("../test/driver.js")
require("../test/tests-class-features-2022.js")

// Six existing private-name parse checks, selected from
// test/tests-class-features-2022.js by their exact source strings.
const BASELINE_CODES = [
  "class C { #a; #a }", // duplicate private declaration
  "class C { get #a(){} static set #a(x){} }", // static/instance accessor cross
  "class C { #a; a = this.#b; }", // undeclared use in field initializer
  "class C { #a; f(){ this.#b } }", // undeclared use in method body
  "obj.#aaa", // private use outside any class
  "class Outer { Inner = class { f(obj) { obj.#nonexist } #inner; }; #outer; }" // undeclared use in nested class
]

const wanted = new Set(BASELINE_CODES)
const {parse} = await loadParser()

let ran = 0
let failed = 0
driver.runTests({parse, filter: test => wanted.has(test.code)}, (status, code, message) => {
  ran++
  if (status === "ok") {
    process.stdout.write(`baseline ok: ${code}\n`)
  } else {
    failed++
    process.stdout.write(`baseline FAIL: ${code}\n  ${message || "unexpected status " + status}\n`)
  }
})

if (ran !== BASELINE_CODES.length) {
  process.stdout.write(`expected ${BASELINE_CODES.length} baseline cases, but ${ran} ran\n`)
  process.exit(1)
}
if (failed > 0) process.exit(1)
process.stdout.write(`ACORN_PRIVATE_BASELINE_OK cases=${ran}\n`)
