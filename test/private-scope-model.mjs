// Model driven private name scope regression tooling.
//
// The private name environment of a program is described by an explicit
// model (PrivateScopeModel) before any source code is rendered. Acorn is
// only consulted afterwards to check that its behaviour matches the model.
// No eval, Function, vm or host-engine compilation is used anywhere here:
// generated strings are only fed to Parser.parse, never executed.

import {cp, mkdtemp, readFile, rm, writeFile} from "node:fs/promises"
import {tmpdir} from "node:os"
import {dirname, join, resolve} from "node:path"
import {fileURLToPath, pathToFileURL} from "node:url"
import {loadParser} from "../baseline/load-parser.mjs"

export const PARSE_OPTIONS = {
  ecmaVersion: 2022,
  checkPrivateFields: true,
  locations: true
}

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..")

// ---------------------------------------------------------------------------
// Deterministic pseudo random number generation (mulberry32)
// ---------------------------------------------------------------------------

export function mulberry32(seed) {
  let a = seed >>> 0
  return function next() {
    a |= 0
    a = (a + 0x6D2B79F5) | 0
    let t = Math.imul(a ^ (a >>> 15), 1 | a)
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296
  }
}

export class Rng {
  constructor(seed) {
    this.seed = seed
    this._next = mulberry32(seed)
  }
  next() {
    return this._next()
  }
  int(n) {
    return Math.floor(this._next() * n)
  }
  pick(list) {
    return list[this.int(list.length)]
  }
  bool(probability = 0.5) {
    return this._next() < probability
  }
  shuffle(list) {
    for (let i = list.length - 1; i > 0; --i) {
      const j = this.int(i + 1)
      ;[list[i], list[j]] = [list[j], list[i]]
    }
    return list
  }
}

// ---------------------------------------------------------------------------
// The independent class environment model
// ---------------------------------------------------------------------------

let idCounter = 0
function nextId(prefix) {
  return prefix + (idCounter++).toString(36)
}

// A ClassEnv is one private name environment. Ordinary functions, arrow
// functions, methods and class static blocks deliberately have no
// counterpart here: they do not create private name environments.
class ClassEnv {
  constructor(model, parent, depth, id = nextId("cls")) {
    this.model = model
    this.id = id
    this.parent = parent
    this.child = null
    this.depth = depth
    this.className = "C" + model.classes.length
    // {id, name, kind: "field"|"method"|"get"|"set", isStatic}
    this.declarations = []
    this.declIndex = new Map()
    // {id, name, region: "body"|"heritage", form: "member"|"in", site}
    this.uses = []
  }

  addDeclaration(name, kind, isStatic, id = nextId("dcl")) {
    const decl = {id, name, kind, isStatic}
    this.declarations.push(decl)
    if (!this.declIndex.has(name)) this.declIndex.set(name, [])
    this.declIndex.get(name).push(decl)
    this.model.privateNames.add(name)
    return decl
  }

  addUse(name, region, form, site, id = nextId("use")) {
    const use = {id, name, region, form, site}
    this.uses.push(use)
    return use
  }

  heritageUse() {
    return this.uses.find(use => use.region === "heritage") || null
  }
}

export class PrivateScopeModel {
  constructor(seed = 0) {
    this.seed = seed
    this.classes = []
    this.root = null
    this.privateNames = new Set()
    this.nameCounter = 0
  }

  addClass(parent, depth, id) {
    const env = new ClassEnv(this, parent, depth, id)
    if (parent) parent.child = env
    else this.root = env
    this.classes.push(env)
    return env
  }

  freshName() {
    const name = "n" + this.nameCounter++
    this.privateNames.add(name)
    return name
  }

  // Resolution rule of the model:
  //  - body uses see the class itself and every enclosing class,
  //  - a heritage (extends) expression is evaluated before the class body
  //    is entered, so resolution starts at the parent environment,
  //  - nested class declarations shadow outer names of the same spelling.
  resolve(classEnv, region, name) {
    let env = region === "heritage" ? classEnv.parent : classEnv
    while (env) {
      if (env.declIndex.has(name)) return env
      env = env.parent
    }
    return null
  }

  // Independent re-implementation of the declaration conflict rule that
  // acorn encodes in isPrivateNameConflicted(): a get/set pair is only
  // legal when the names match, static-ness matches and each side exists
  // at most once; fields, methods, duplicate accessors and static/instance
  // crossing all conflict.
  conflictFor(classEnv) {
    const state = new Map()
    for (const decl of classEnv.declarations) {
      const name = decl.name
      const next = (decl.kind === "get" || decl.kind === "set")
        ? (decl.isStatic ? "s" : "i") + decl.kind
        : "true"
      const curr = state.get(name)
      const paired =
        curr === "iget" && next === "iset" ||
        curr === "iset" && next === "iget" ||
        curr === "sget" && next === "sset" ||
        curr === "sset" && next === "sget"
      if (paired) {
        state.set(name, "true")
      } else if (!curr) {
        state.set(name, next)
      } else {
        const earlier = classEnv.declarations.find(d => d.name === name && d !== decl)
        return {name, later: decl, earlier: earlier || null}
      }
    }
    return null
  }

  // The model's own verdict, without consulting any parser.
  expectedError() {
    for (const classEnv of this.classes) {
      const conflict = this.conflictFor(classEnv)
      if (conflict) {
        return {
          kind: "conflict",
          markerId: conflict.later.id,
          name: conflict.name,
          classId: classEnv.id
        }
      }
    }
    for (const classEnv of this.classes) {
      for (const use of classEnv.uses) {
        if (!this.resolve(classEnv, use.region, use.name)) {
          return {
            kind: "undeclared",
            markerId: use.id,
            name: use.name,
            classId: classEnv.id
          }
        }
      }
    }
    return null
  }

  get accepted() {
    return this.expectedError() === null
  }

  *allMarkers() {
    for (const classEnv of this.classes) {
      for (const decl of classEnv.declarations) yield {kind: "declaration", id: decl.id, name: decl.name}
      for (const use of classEnv.uses) yield {kind: "use", id: use.id, name: use.name}
    }
  }

  findMarker(id) {
    for (const classEnv of this.classes) {
      for (const decl of classEnv.declarations) if (decl.id === id) return decl
      for (const use of classEnv.uses) if (use.id === id) return use
    }
    return null
  }
}

export function cloneModel(model) {
  const copy = new PrivateScopeModel(model.seed)
  copy.nameCounter = model.nameCounter
  for (const name of model.privateNames) copy.privateNames.add(name)
  const envMap = new Map()
  for (const classEnv of model.classes) {
    const cloned = copy.addClass(
      classEnv.parent ? envMap.get(classEnv.parent) : null,
      classEnv.depth,
      classEnv.id
    )
    cloned.className = classEnv.className
    envMap.set(classEnv, cloned)
  }
  for (const classEnv of model.classes) {
    const cloned = envMap.get(classEnv)
    for (const decl of classEnv.declarations) {
      cloned.addDeclaration(decl.name, decl.kind, decl.isStatic, decl.id)
    }
    for (const use of classEnv.uses) {
      cloned.addUse(use.name, use.region, use.form, use.site, use.id)
    }
  }
  return copy
}

// ---------------------------------------------------------------------------
// Source rendering. Every declaration and every private use is tagged with
// its rendered UTF-16 code unit range while the text is built.
// ---------------------------------------------------------------------------

class SourceBuilder {
  constructor() {
    this.parts = []
    this.length = 0
    this.ranges = new Map()
  }
  raw(text) {
    this.parts.push(text)
    this.length += text.length
    return this
  }
  mark(id, text) {
    const start = this.length
    this.parts.push(text)
    this.length += text.length
    this.ranges.set(id, {start, end: this.length})
    return this
  }
  render() {
    return this.parts.join("")
  }
}

function tokenText(name, escapedNames) {
  return escapedNames && escapedNames.has(name) ? "#\\u0041" : "#" + name
}

function renderUse(builder, use, escapedNames) {
  if (use.form === "member") {
    builder.raw("o.")
    builder.mark(use.id, tokenText(use.name, escapedNames))
  } else {
    builder.mark(use.id, tokenText(use.name, escapedNames))
    builder.raw(" in o")
  }
}

function renderClass(builder, classEnv, escapedNames) {
  const heritage = classEnv.heritageUse()
  builder.raw("class " + classEnv.className)
  if (heritage) {
    builder.raw(" extends (")
    renderUse(builder, heritage, escapedNames)
    builder.raw(")")
  }
  builder.raw("{")
  for (const decl of classEnv.declarations) {
    if (decl.isStatic) builder.raw("static ")
    if (decl.kind === "get" || decl.kind === "set") builder.raw(decl.kind + " ")
    builder.mark(decl.id, tokenText(decl.name, escapedNames))
    if (decl.kind === "field") builder.raw(";")
    else if (decl.kind === "method") builder.raw("(){}")
    else builder.raw(decl.kind === "get" ? "(){}" : "(v){}")
  }
  let fieldIndex = 0
  for (const use of classEnv.uses) {
    if (use.site !== "field") continue
    builder.raw("fd" + fieldIndex++ + "=")
    renderUse(builder, use, escapedNames)
    builder.raw(";")
  }
  const staticUses = classEnv.uses.filter(use => use.site === "static")
  if (staticUses.length) {
    builder.raw("static{")
    for (const use of staticUses) {
      renderUse(builder, use, escapedNames)
      builder.raw(";")
    }
    builder.raw("}")
  }
  const methodUses = classEnv.uses.filter(use =>
    use.site === "method" || use.site === "arrow" || use.site === "function")
  if (methodUses.length || classEnv.child) {
    builder.raw("m(){")
    let bindingIndex = 0
    for (const use of methodUses) {
      if (use.site === "arrow") {
        builder.raw("const f" + bindingIndex + "=(o)=>")
        renderUse(builder, use, escapedNames)
        builder.raw(";")
      } else if (use.site === "function") {
        builder.raw("const g" + bindingIndex + "=function(o){return ")
        renderUse(builder, use, escapedNames)
        builder.raw(";};")
      } else {
        builder.raw("return ")
        renderUse(builder, use, escapedNames)
        builder.raw(";")
      }
      bindingIndex++
    }
    if (classEnv.child) {
      builder.raw("return ")
      renderClass(builder, classEnv.child, escapedNames)
      builder.raw(";")
    }
    builder.raw("}")
  }
  // Deliberate non-private decoys: a normal member access and a string that
  // merely contains the characters "#x". Neither is a private name use.
  builder.raw("d(){const tag=\"#x\";const o={};return o.x;}")
  builder.raw("}")
}

export function renderProgram(model, options = {}) {
  const builder = new SourceBuilder()
  renderClass(builder, model.root, options.escapedNames || null)
  return {source: builder.render(), ranges: new Map(builder.ranges)}
}

// ---------------------------------------------------------------------------
// Generators: relationships first, rendering second.
// ---------------------------------------------------------------------------

const BODY_SITES = ["method", "arrow", "function", "field", "static"]
const MUTANT_TYPES = ["undeclared", "heritage", "duplicate", "accessor", "outer"]

function envChain(classEnv) {
  const chain = []
  for (let env = classEnv; env; env = env.parent) chain.push(env)
  return chain
}

export function generateAcceptedModel(rng, options = {}) {
  const maxDepth = options.maxDepth || 3
  const model = new PrivateScopeModel(options.seed || 0)
  const depth = 1 + rng.int(maxDepth)
  let parent = null
  for (let d = 0; d < depth; ++d) parent = model.addClass(parent, d)

  for (const classEnv of model.classes) {
    const declCount = 1 + rng.int(2)
    for (let i = 0; i < declCount; ++i) {
      let name
      // Sometimes shadow a name declared by an enclosing class.
      if (i === 0 && classEnv.parent && rng.bool(0.4)) {
        name = rng.pick(classEnv.parent.declarations).name
      } else {
        name = model.freshName()
      }
      const kind = rng.pick(["field", "method", "get", "set"])
      const isStatic = rng.bool(0.4)
      classEnv.addDeclaration(name, kind, isStatic)
      if ((kind === "get" || kind === "set") && rng.bool(0.5)) {
        classEnv.addDeclaration(name, kind === "get" ? "set" : "get", isStatic)
      }
    }
  }

  for (const classEnv of model.classes) {
    const useCount = 1 + rng.int(2)
    for (let i = 0; i < useCount; ++i) {
      const heritage = !!classEnv.parent && !classEnv.heritageUse() && rng.bool(0.3)
      if (heritage) {
        // The heritage clause can only see enclosing class environments.
        const targetEnv = rng.pick(envChain(classEnv.parent))
        const decl = rng.pick(targetEnv.declarations)
        classEnv.addUse(decl.name, "heritage", rng.pick(["member", "in"]), "heritage")
      } else {
        const targetEnv = rng.pick(envChain(classEnv))
        const decl = rng.pick(targetEnv.declarations)
        classEnv.addUse(decl.name, "body", rng.pick(["member", "in"]), rng.pick(BODY_SITES))
      }
    }
  }
  return model
}

// A base model purpose-built for the "outer" fault: the outer class owns a
// private declaration that is referenced exactly once, by the nested class,
// and nowhere else, so deleting that declaration is a single controlled
// break of outer-class environment lookup.
export function generateOuterRemovableModel(rng, options = {}) {
  const model = new PrivateScopeModel(options.seed || 0)
  const outer = model.addClass(null, 0)
  const inner = model.addClass(outer, 1)
  const kept = model.freshName()
  const removable = model.freshName()
  const innerName = model.freshName()
  outer.addDeclaration(kept, rng.pick(["field", "method"]), rng.bool(0.3))
  outer.addDeclaration(removable, "field", false)
  outer.addUse(kept, "body", rng.pick(["member", "in"]), rng.pick(BODY_SITES))
  inner.addDeclaration(innerName, "field", false)
  inner.addUse(removable, "body", rng.pick(["member", "in"]), rng.pick(BODY_SITES))
  inner.addUse(innerName, "body", "member", "method")
  return model
}

// Apply exactly one controlled fault to an accepted model. Returns null when
// the preconditions for the requested fault type are not met.
export function mutateModel(model, rng, type) {
  const mutant = cloneModel(model)
  if (type === "undeclared") {
    const cls = rng.pick(mutant.classes)
    const use = rng.pick(cls.uses)
    use.name = mutant.freshName()
    return mutant.accepted ? null : mutant
  }

  if (type === "heritage") {
    const candidates = mutant.classes.filter(cls => {
      if (!cls.parent) return false
      return cls.declarations.some(decl =>
        !mutant.resolve(cls.parent, "body", decl.name))
    })
    if (!candidates.length) return null
    const cls = rng.pick(candidates)
    if (cls.heritageUse()) return null
    const decl = rng.pick(cls.declarations.filter(d =>
      !mutant.resolve(cls.parent, "body", d.name)))
    cls.addUse(decl.name, "heritage", rng.pick(["member", "in"]), "heritage")
    return mutant.accepted ? null : mutant
  }

  if (type === "duplicate") {
    const cls = rng.pick(mutant.classes)
    const decl = rng.pick(cls.declarations)
    cls.addDeclaration(decl.name, "field", rng.bool(0.5))
    return mutant.accepted ? null : mutant
  }

  if (type === "accessor") {
    const paired = []
    for (const cls of mutant.classes) {
      for (const decl of cls.declarations) {
        if (decl.kind !== "get" && decl.kind !== "set") continue
        const mate = cls.declarations.find(d =>
          d !== decl && d.name === decl.name &&
          (d.kind === "get" || d.kind === "set") && d.isStatic === decl.isStatic)
        if (mate) paired.push({cls, decl})
      }
    }
    if (!paired.length) return null
    const {decl} = rng.pick(paired)
    decl.isStatic = !decl.isStatic
    return mutant.accepted ? null : mutant
  }

  if (type === "outer") {
    // Find an outer-class declaration whose removal introduces exactly one
    // orphaned use. The declaration must be the sole declaration of that
    // name in its class (no get/set mate), no ancestor may declare the
    // name, the owning class itself must not use it, and exactly one nested
    // class may reference it.
    const references = new Map()
    const keyOf = (cls, decl) => cls.id + "|" + decl.id
    for (const cls of mutant.classes) {
      for (const use of cls.uses) {
        const owner = mutant.resolve(cls, use.region, use.name)
        if (!owner || owner === cls) continue
        const decl = owner.declIndex.get(use.name)[0]
        const key = keyOf(owner, decl)
        if (!references.has(key)) references.set(key, {owner, decl, uses: []})
        references.get(key).uses.push({cls, use})
      }
    }
    const candidates = [...references.values()].filter(entry => {
      if (entry.uses.length !== 1) return false
      if (entry.owner.declIndex.get(entry.decl.name).length !== 1) return false
      if (entry.owner.uses.some(use => use.name === entry.decl.name)) return false
      return !mutant.resolve(entry.owner.parent, "body", entry.decl.name)
    })
    if (!candidates.length) return null
    const entry = rng.pick(candidates)
    const targetClass = mutant.classes.find(cls => cls.id === entry.owner.id)
    const targetDecl = targetClass.declarations.find(d => d.id === entry.decl.id)
    targetClass.declarations.splice(targetClass.declarations.indexOf(targetDecl), 1)
    const sameName = targetClass.declIndex.get(targetDecl.name)
    sameName.splice(sameName.indexOf(targetDecl), 1)
    if (sameName.length === 0) targetClass.declIndex.delete(targetDecl.name)
    const error = mutant.expectedError()
    if (error && error.kind === "undeclared" && error.markerId === entry.uses[0].use.id) {
      return mutant
    }
    return null
  }

  throw new Error("Unknown mutant type: " + type)
}

// ---------------------------------------------------------------------------
// Transformations: rename, declaration reordering, whitespace/comment noise.
// Each returns a fresh model/source/ranges triple.
// ---------------------------------------------------------------------------

export function transformRename(model, rng) {
  const copy = cloneModel(model)
  const from = rng.pick([...copy.privateNames])
  const to = copy.freshName()
  copy.privateNames.delete(from)
  for (const cls of copy.classes) {
    for (const decl of cls.declarations) if (decl.name === from) decl.name = to
    for (const use of cls.uses) if (use.name === from) use.name = to
    if (cls.declIndex.has(from)) {
      cls.declIndex.set(to, cls.declIndex.get(from))
      cls.declIndex.delete(from)
    }
  }
  const rendered = renderProgram(copy)
  return {model: copy, ...rendered, transform: "rename", detail: {from, to}}
}

export function transformReorder(model, rng) {
  const copy = cloneModel(model)
  for (const cls of copy.classes) {
    if (cls.declarations.length > 1) rng.shuffle(cls.declarations)
  }
  const rendered = renderProgram(copy)
  return {model: copy, ...rendered, transform: "reorder"}
}

function insertEdits(source, ranges, edits) {
  const sorted = [...edits].sort((a, b) => a.pos - b.pos)
  const nextRanges = new Map()
  for (const [id, range] of ranges) {
    let start = range.start
    let end = range.end
    for (const edit of sorted) {
      if (edit.pos <= range.start) {
        start += edit.text.length
        end += edit.text.length
      } else if (edit.pos < range.end) {
        end += edit.text.length
      }
    }
    nextRanges.set(id, {start, end})
  }
  let out = ""
  let cursor = 0
  for (const edit of sorted) {
    out += source.slice(cursor, edit.pos) + edit.text
    cursor = edit.pos
  }
  out += source.slice(cursor)
  return {source: out, ranges: nextRanges}
}

export function transformSpacing(model, rendered, rng) {
  const gapPositions = new Set()
  for (let i = 0; i < rendered.source.length; ++i) {
    const ch = rendered.source[i]
    if (ch === "{" || ch === ";") gapPositions.add(i + 1)
    if (ch === ".") gapPositions.add(i)
  }
  for (const range of rendered.ranges.values()) {
    for (let p = range.start; p < range.end; ++p) gapPositions.delete(p)
  }
  const positions = [...gapPositions]
  const editCount = Math.min(positions.length, 1 + rng.int(3))
  const edits = rng.shuffle(positions).slice(0, editCount).map(pos => ({
    pos,
    text: rng.pick([" ", "\n", "/*q*/"])
  }))
  const result = insertEdits(rendered.source, rendered.ranges, edits)
  return {model, source: result.source, ranges: result.ranges, transform: "spacing", edits}
}

// Fixed boundary: a legal private identifier written with a Unicode escape.
export function unicodeEscapeFixture() {
  const model = new PrivateScopeModel(0)
  const cls = model.addClass(null, 0)
  cls.addDeclaration("A", "field", false)
  cls.addUse("A", "body", "member", "method")
  const rendered = renderProgram(model, {escapedNames: new Set(["A"])})
  return {model, ...rendered}
}

// ---------------------------------------------------------------------------
// Oracle: compare the model verdict against a real, full Parser.parse run.
// Tokenizer success is never accepted as a substitute for parsing.
// ---------------------------------------------------------------------------

function collectPrivateIdentifiers(node, out = []) {
  if (!node || typeof node !== "object") return out
  if (node.type === "PrivateIdentifier") {
    out.push({name: node.name, start: node.start, end: node.end})
  }
  for (const key of Object.keys(node)) {
    if (key === "loc" || key === "start" || key === "end" || key === "range") continue
    const value = node[key]
    if (Array.isArray(value)) for (const child of value) collectPrivateIdentifiers(child, out)
    else if (value && typeof value.type === "string") collectPrivateIdentifiers(value, out)
  }
  return out
}

export function verifyAccepted(parser, model, source, ranges) {
  const ast = parser.parse(source, PARSE_OPTIONS)
  if (!ast || ast.type !== "Program") {
    throw new Error("Parser.parse did not return a Program node")
  }
  const expected = [...model.allMarkers()]
    .map(marker => ({name: marker.name, ...ranges.get(marker.id)}))
    .sort((a, b) => a.start - b.start)
  const actual = collectPrivateIdentifiers(ast).sort((a, b) => a.start - b.start)
  if (actual.length !== expected.length) {
    return {
      ok: false,
      reason: `PrivateIdentifier count ${actual.length} !== model marker count ${expected.length}`,
      actual,
      expected
    }
  }
  for (let i = 0; i < actual.length; ++i) {
    if (actual[i].name !== expected[i].name ||
        actual[i].start !== expected[i].start ||
        actual[i].end !== expected[i].end) {
      return {
        ok: false,
        reason: "PrivateIdentifier name/range mismatch",
        actual: actual[i],
        expected: expected[i]
      }
    }
  }
  return {ok: true}
}

export function verifyRejected(parser, model, source, ranges) {
  const expectedError = model.expectedError()
  if (!expectedError) return {ok: false, reason: "model expected acceptance, not rejection"}
  let thrown
  try {
    parser.parse(source, PARSE_OPTIONS)
  } catch (error) {
    thrown = error
  }
  if (!thrown) return {ok: false, reason: "parser accepted a program the model rejects", expectedError}
  if (!(thrown instanceof SyntaxError) || !Number.isInteger(thrown.pos)) {
    return {ok: false, reason: "parser did not raise a positioned SyntaxError", thrown: String(thrown)}
  }
  const range = ranges.get(expectedError.markerId)
  if (!range) return {ok: false, reason: "model marker has no rendered range", expectedError}
  if (thrown.pos < range.start || thrown.pos >= range.end) {
    return {
      ok: false,
      reason: "SyntaxError.pos does not point at the controlled fault marker",
      pos: thrown.pos,
      range,
      expectedError
    }
  }
  return {ok: true, pos: thrown.pos, expectedError}
}

export function verifyAgainstModel(parser, model, source, ranges) {
  return model.accepted
    ? verifyAccepted(parser, model, source, ranges)
    : verifyRejected(parser, model, source, ranges)
}

// ---------------------------------------------------------------------------
// Fuzz driver
// ---------------------------------------------------------------------------

export function defaultCliArgs(argv = process.argv.slice(2)) {
  const args = {seed: 1320, cases: 160}
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === "--seed") args.seed = Number(argv[++i])
    else if (argv[i] === "--cases") args.cases = Number(argv[++i])
  }
  if (!Number.isInteger(args.seed) || !Number.isInteger(args.cases) || args.cases <= 0) {
    throw new Error("usage: --seed <integer> --cases <positive integer>")
  }
  return args
}

export async function runFuzz(parser, options = {}) {
  const seed = options.seed ?? 1320
  const cases = options.cases ?? 160
  const rng = new Rng(seed)
  const counts = {
    seed,
    cases,
    accepted: 0,
    rejected: 0,
    mutants: Object.fromEntries(MUTANT_TYPES.map(type => [type, 0])),
    transforms: {rename: 0, reorder: 0, spacing: 0},
    sites: Object.fromEntries(BODY_SITES.map(site => [site, 0])),
    heritage: 0,
    shadow: 0,
    accessorPairs: 0,
    depths: {},
    divergences: []
  }

  const recordStructure = model => {
    counts.depths[model.classes.length] = (counts.depths[model.classes.length] || 0) + 1
    for (const cls of model.classes) {
      if (cls.heritageUse()) counts.heritage++
      if (cls.parent && cls.declarations.some(d => model.resolve(cls.parent, "body", d.name))) {
        counts.shadow++
      }
      for (const use of cls.uses) if (use.region === "body") counts.sites[use.site]++
      const names = new Set(cls.declarations.map(d => d.name))
      for (const name of names) {
        const hasGet = cls.declarations.some(d => d.name === name && d.kind === "get")
        const hasSet = cls.declarations.some(d => d.name === name && d.kind === "set")
        if (hasGet && hasSet) counts.accessorPairs++
      }
    }
  }

  const check = (model, source, ranges, label) => {
    const result = verifyAgainstModel(parser, model, source, ranges)
    if (!result.ok) {
      counts.divergences.push({label, source, model: snapshotModel(model), seed, result,
        firstErrorRange: model.expectedError() ? ranges.get(model.expectedError().markerId) : null})
    }
    return result
  }

  let mutantCursor = 0
  for (let i = 0; i < cases; ++i) {
    const base = generateAcceptedModel(rng, {seed, maxDepth: 3})
    if (!base.accepted) throw new Error("generator produced a rejected base model")
    const rendered = renderProgram(base)

    if (i % 2 === 0) {
      recordStructure(base)
      const result = check(base, rendered.source, rendered.ranges, "accepted#" + i)
      if (result.ok) counts.accepted++
      for (const transform of [transformRename(base, rng),
                               transformReorder(base, rng),
                               transformSpacing(base, rendered, rng)]) {
        const transformed = check(transform.model, transform.source, transform.ranges,
          "accepted#" + i + ":" + transform.transform)
        if (transformed.ok) counts.transforms[transform.transform]++
      }
    } else {
      const scheduled = MUTANT_TYPES[mutantCursor % MUTANT_TYPES.length]
      mutantCursor++
      let mutant = null
      let chosenType = null
      if (scheduled === "outer") {
        // The generic random base rarely satisfies the single-reference
        // precondition, so the outer lookup fault gets a purpose-built base.
        const outerBase = generateOuterRemovableModel(rng, {seed})
        if (!outerBase.accepted) throw new Error("outer base model is not accepted")
        recordStructure(outerBase)
        mutant = mutateModel(outerBase, rng, "outer")
        chosenType = "outer"
      } else {
        const order = [scheduled, ...MUTANT_TYPES.filter(t => t !== scheduled && t !== "outer")]
        for (const type of order) {
          mutant = mutateModel(base, rng, type)
          if (mutant) {
            chosenType = type
            break
          }
        }
        recordStructure(base)
      }
      if (!mutant) throw new Error("could not construct any single-fault mutant for case " + i)
      counts.mutants[chosenType]++
      const mutantRendered = renderProgram(mutant)
      const result = check(mutant, mutantRendered.source, mutantRendered.ranges,
        "mutant#" + i + ":" + chosenType)
      if (result.ok) counts.rejected++
    }
  }

  // Fixed Unicode escape boundary case.
  const unicode = unicodeEscapeFixture()
  const unicodeResult = verifyAccepted(parser, unicode.model, unicode.source, unicode.ranges)
  if (!unicodeResult.ok) {
    counts.divergences.push({
      label: "unicode-escape-fixture",
      source: unicode.source,
      model: snapshotModel(unicode.model),
      seed,
      result: unicodeResult,
      firstErrorRange: null
    })
  }
  counts.unicodeFixture = unicodeResult.ok ? 1 : 0
  return counts
}

function snapshotModel(model) {
  return {
    seed: model.seed,
    classes: model.classes.map(cls => ({
      id: cls.id,
      parent: cls.parent ? cls.parent.id : null,
      depth: cls.depth,
      declarations: cls.declarations.map(d => ({...d})),
      uses: cls.uses.map(u => ({...u}))
    }))
  }
}

export async function writeDivergenceArtifacts(counts) {
  const files = []
  for (let i = 0; i < counts.divergences.length; ++i) {
    const divergence = counts.divergences[i]
    const file = join(tmpdir(), `acorn-private-divergence-${process.pid}-${i}.json`)
    await writeFile(file, JSON.stringify(divergence, null, 2))
    files.push(file)
  }
  return files
}

export function formatCounts(counts) {
  return [
    `model seed=${counts.seed} cases=${counts.cases}`,
    `accepted=${counts.accepted} rejected=${counts.rejected}`,
    `mutants=${JSON.stringify(counts.mutants)}`,
    `transforms=${JSON.stringify(counts.transforms)}`,
    `sites=${JSON.stringify(counts.sites)}`,
    `heritage=${counts.heritage} shadow=${counts.shadow} accessorPairs=${counts.accessorPairs}`,
    `depths=${JSON.stringify(counts.depths)} unicodeFixture=${counts.unicodeFixture}`,
    `divergences=${counts.divergences.length}`
  ].join("\n")
}

// ---------------------------------------------------------------------------
// Mutation testing against temporary patched copies of the parser sources.
// The working tree sources are never modified.
// ---------------------------------------------------------------------------

export const PARSER_PATCHES = [
  {
    name: "outer-private-env-lookup",
    find: "      if (parent) {\n        parent.used.push(id)\n      } else {",
    replace: "      if (parent) {\n        this.raiseRecoverable(id.start, `Private field '#${id.name}' must be declared in an enclosing class`)\n      } else {"
  },
  {
    name: "accessor-static-pairing",
    find: "    curr === \"sget\" && next === \"sset\" ||\n    curr === \"sset\" && next === \"sget\"",
    replace: "    false ||\n    false"
  },
  {
    name: "heritage-class-body-boundary",
    find: "  this.parseClassId(node, isStatement)\n  this.parseClassSuper(node)\n  const privateNameMap = this.enterClassBody()",
    replace: "  this.parseClassId(node, isStatement)\n  const privateNameMap = this.enterClassBody()\n  this.parseClassSuper(node)"
  }
]

export async function loadPatchedParser(patch) {
  const dir = await mkdtemp(join(tmpdir(), "acorn-private-mutant-"))
  await cp(join(REPO_ROOT, "acorn/src"), join(dir, "src"), {recursive: true})
  await writeFile(join(dir, "package.json"), "{\"type\":\"module\"}\n")
  const statementFile = join(dir, "src/statement.js")
  const original = await readFile(statementFile, "utf8")
  const occurrences = original.split(patch.find).length - 1
  if (occurrences !== 1) {
    await rm(dir, {recursive: true, force: true})
    throw new Error(`patch ${patch.name}: expected one target site, found ${occurrences}`)
  }
  await writeFile(statementFile, original.replace(patch.find, patch.replace))
  const entry = pathToFileURL(join(dir, "src/index.js")).href
  const parser = await import(entry)
  return {
    parser,
    dir,
    dispose: () => rm(dir, {recursive: true, force: true})
  }
}

export async function loadBaselineParser() {
  return loadParser(REPO_ROOT)
}

// Direct CLI: node test/private-scope-model.mjs --seed 1320 --cases 160
const invokedPath = process.argv[1] ? resolve(process.argv[1]) : ""
if (import.meta.url === pathToFileURL(invokedPath).href) {
  const args = defaultCliArgs()
  const loaded = await loadBaselineParser()
  try {
    const counts = await runFuzz(loaded.parser, args)
    console.log(formatCounts(counts))
    if (counts.divergences.length) {
      const files = await writeDivergenceArtifacts(counts)
      console.error("divergence artifacts: " + files.join(", "))
      process.exitCode = 1
    }
  } finally {
    await loaded.dispose()
  }
}
