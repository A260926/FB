// Model-driven private-name syntax regression for acorn.
//
// This module builds an explicit model of class private-name scopes
// (declarations, uses, nesting, heritage expressions), decides legality
// on its own, and only then renders JavaScript source text. The rendered
// programs -- never executed, only parsed -- are fed to the real parser
// loaded via baseline/load-parser.mjs, and the parser's verdicts and
// positions are compared against the model's expectations.
//
// Usage: node test/private-scope-model.mjs --seed 1320 --cases 160

import {writeFileSync} from "node:fs"
import {tmpdir} from "node:os"
import {join} from "node:path"
import {pathToFileURL} from "node:url"
import {loadParser} from "../baseline/load-parser.mjs"

// ---------------------------------------------------------------------
// Deterministic PRNG (mulberry32)

export function makeRng(seed) {
  let a = seed >>> 0
  return function() {
    a |= 0; a = a + 0x6D2B79F5 | 0
    let t = Math.imul(a ^ a >>> 15, 1 | a)
    t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t
    return ((t ^ t >>> 14) >>> 0) / 4294967296
  }
}

function randInt(rng, n) {
  return Math.floor(rng() * n)
}

function pick(rng, arr) {
  return arr[randInt(rng, arr.length)]
}

// ---------------------------------------------------------------------
// The scope model.
//
// A Prog holds a tree of class environments. Every class environment has
// a stable identity and records its private declarations and the private
// uses that belong to it. Plain functions, arrow functions, methods and
// static blocks do NOT create private-name environments: uses inside
// them are registered on the enclosing class environment. A class's
// heritage (extends) expression belongs to the environment in effect
// before the class body is entered, i.e. the parent environment.

export const PARSE_OPTIONS = {ecmaVersion: 2022, checkPrivateFields: true}

const DECL_NAME_POOL = ["a", "b", "c", "x", "y", "z", "p", "q", "g", "s"]

class Prog {
  constructor() {
    this.envs = []
    this.decls = []
    this.uses = []
    this.topLevel = []
    this._fun = 0
    this._pub = 0
    this._und = 0
    this._extra = 0
  }

  newEnv(parent, className) {
    const env = new ClassEnv(this, parent, className)
    this.envs.push(env)
    return env
  }

  newDecl(env, name, kind, isStatic) {
    const decl = {id: this.decls.length, env, name, kind, static: !!isStatic, range: null}
    env.decls.push(decl)
    this.decls.push(decl)
    return decl
  }

  newUse(env, name, form, obj) {
    const use = {id: this.uses.length, env, name, form, obj, range: null}
    this.uses.push(use)
    return use
  }

  freshUndeclared() { return `u${this._und++}` }
  freshFunctionName() { return `f${this._fun++}` }
  freshPublicName() { return `p${this._pub++}` }
  freshExtraName() { return `n${this._extra++}` }
}

class ClassEnv {
  constructor(prog, parent, className) {
    this.prog = prog
    this.id = prog.envs.length
    this.parent = parent
    this.className = className
    this.elements = [] // class body elements, in render order
    this.decls = [] // private declarations, in creation order
    this.heritage = null // extends expression fragment, or null
  }

  declaresName(name) {
    return this.decls.some(d => d.name === name)
  }

  declaredNames() {
    return [...new Set(this.decls.map(d => d.name))]
  }
}

// ---------------------------------------------------------------------
// Model construction helpers

function pickFreshDeclName(prog, env, rng) {
  const taken = new Set(env.declaredNames())
  const available = DECL_NAME_POOL.filter(n => !taken.has(n))
  if (available.length) return pick(rng, available)
  let name = prog.freshExtraName()
  while (taken.has(name)) name = prog.freshExtraName()
  return name
}

// Element builders. Elements render in the order they are pushed.

function addField(prog, env, name, {isStatic = false, init = null} = {}) {
  const decl = prog.newDecl(env, name, "field", isStatic)
  const el = {type: "field", decl, init}
  env.elements.push(el)
  return {decl, el}
}

function addMethod(prog, env, name, {isStatic = false, body = []} = {}) {
  const decl = prog.newDecl(env, name, "method", isStatic)
  const el = {type: "method", decl, publicName: null, body}
  env.elements.push(el)
  return {decl, el}
}

function addPublicMethod(prog, env, {isStatic = false, body = []} = {}) {
  const el = {type: "method", decl: null, publicName: prog.freshPublicName(), body}
  env.elements.push(el)
  return {el}
}

function addAccessor(prog, env, name, kind, {isStatic = false, body = []} = {}) {
  const decl = prog.newDecl(env, name, kind, isStatic)
  const el = {type: "accessor", decl, body}
  env.elements.push(el)
  return {decl, el}
}

function addStaticBlock(prog, env, body) {
  const el = {type: "staticBlock", body}
  env.elements.push(el)
  return {el}
}

function addPublicField(prog, env, init) {
  const el = {type: "publicField", name: prog.freshPublicName(), init}
  env.elements.push(el)
  return {el}
}

function addComputedField(prog, env, keyUse, init) {
  const el = {type: "computed", keyUse, init}
  env.elements.push(el)
  return {el}
}

// Fragment constructors. Uses created here are registered on `env`,
// which the generator chooses according to the scoping rules.

function lit(text) {
  return {e: "lit", text}
}

function useExpr(use) {
  return {e: "use", use}
}

function memberUse(prog, env, rng, name) {
  return prog.newUse(env, name, "member", pick(rng, ["this", "obj", "o1"]))
}

function inUse(prog, env, rng, name) {
  return prog.newUse(env, name, "in", pick(rng, ["obj", "o1", "this"]))
}

// Wraps an expression fragment into a statement fragment, exercising
// the fact that functions/arrows do not create private-name scopes.
function wrapExprStmt(prog, rng, expr, allowReturn) {
  const w = rng()
  if (allowReturn && w < 0.3) return {s: "return", e: expr}
  if (w < 0.55) return {s: "expr", e: expr}
  if (w < 0.8) return {s: "expr", e: {e: "arrow", body: [{s: "return", e: expr}]}}
  return {s: "function", name: prog.freshFunctionName(), body: [{s: "return", e: expr}]}
}

function useStmt(prog, env, rng, name, {allowReturn = true} = {}) {
  const use = rng() < 0.6 ? memberUse(prog, env, rng, name) : inUse(prog, env, rng, name)
  return wrapExprStmt(prog, rng, useExpr(use), allowReturn)
}

// A use of `name` in a plain `return this.#x;`-style statement (no rng
// variety), used for the small auditable detection cases.
function plainUseStmt(prog, env, name) {
  const use = prog.newUse(env, name, "member", "this")
  return {s: "return", e: useExpr(use)}
}

// ---------------------------------------------------------------------
// The model checker: decides legality without any parser.

export function resolveUse(use) {
  for (let env = use.env; env; env = env.parent)
    if (env.declaresName(use.name)) return env
  return null
}

function accessorState(decl) {
  if (decl.kind === "get" || decl.kind === "set")
    return (decl.static ? "s" : "i") + decl.kind
  return "true"
}

// Returns a list of problems. Generated error cases contain exactly one.
export function checkModel(prog) {
  const problems = []
  // Declaration conflicts, per class, in render (element) order. Fields
  // and methods clash with any same-name declaration; a getter pairs
  // with a setter only when name, static-ness and cardinality all match.
  for (const env of prog.envs) {
    const states = new Map()
    for (const el of env.elements) {
      const decl = el.decl
      if (!decl) continue
      const next = accessorState(decl)
      const curr = states.get(decl.name)
      if (curr === undefined) {
        states.set(decl.name, next)
        continue
      }
      const pair = curr === "iget" && next === "iset" ||
        curr === "iset" && next === "iget" ||
        curr === "sget" && next === "sset" ||
        curr === "sset" && next === "sget"
      if (pair) {
        states.set(decl.name, "true")
        continue
      }
      problems.push({kind: "conflict", name: decl.name, owner: decl})
    }
  }
  // Undeclared uses: a use resolves along the chain of class
  // environments; inner declarations shadow outer ones.
  for (const use of prog.uses)
    if (!resolveUse(use)) problems.push({kind: "undeclared", name: use.name, owner: use})
  return problems
}

// ---------------------------------------------------------------------
// Renderer: model -> source text, recording UTF-16 ranges (JavaScript
// string indices) of every private name token it emits.

class Renderer {
  constructor() {
    this.code = ""
    this.points = [] // safe whitespace/comment insertion points
  }

  get pos() { return this.code.length }

  emit(text) { this.code += text }

  mark() { this.points.push(this.pos) }

  emitName(owner) {
    const start = this.pos
    this.emit("#" + owner.name)
    owner.range = {start, end: this.pos}
  }
}

function renderProgram(prog) {
  const r = new Renderer()
  for (const stmt of prog.topLevel) renderStmt(r, stmt)
  return {code: r.code, points: r.points}
}

function renderStmt(r, stmt) {
  switch (stmt.s) {
  case "classDecl":
    renderClass(r, stmt.env)
    break
  case "expr":
    renderExpr(r, stmt.e)
    r.emit(";")
    break
  case "return":
    r.emit("return ")
    renderExpr(r, stmt.e)
    r.emit(";")
    break
  case "function":
    r.emit(`function ${stmt.name}() `)
    renderStmtList(r, stmt.body)
    break
  }
}

function renderStmtList(r, stmts) {
  r.emit("{")
  r.mark()
  for (const stmt of stmts) {
    r.emit(" ")
    renderStmt(r, stmt)
    r.mark()
  }
  r.emit("}")
}

function renderClass(r, env) {
  r.emit("class")
  if (env.className) r.emit(" " + env.className)
  if (env.heritage) {
    r.emit(" extends (")
    renderExpr(r, env.heritage)
    r.emit(")")
  }
  r.emit(" {")
  r.mark()
  for (const el of env.elements) {
    r.emit(" ")
    renderElement(r, el)
    r.mark()
  }
  r.emit("}")
}

function renderElement(r, el) {
  switch (el.type) {
  case "field":
    if (el.decl.static) r.emit("static ")
    r.emitName(el.decl)
    if (el.init) {
      r.emit(" = ")
      renderExpr(r, el.init)
    }
    r.emit(";")
    break
  case "method":
    if (el.decl && el.decl.static) r.emit("static ")
    if (el.decl) r.emitName(el.decl)
    else r.emit(el.publicName)
    r.emit("() ")
    renderStmtList(r, el.body)
    break
  case "accessor":
    if (el.decl.static) r.emit("static ")
    r.emit(el.decl.kind + " ")
    r.emitName(el.decl)
    r.emit(el.decl.kind === "get" ? "() " : "(v) ")
    renderStmtList(r, el.body)
    break
  case "staticBlock":
    r.emit("static ")
    renderStmtList(r, el.body)
    break
  case "publicField":
    r.emit(el.name)
    if (el.init) {
      r.emit(" = ")
      renderExpr(r, el.init)
    }
    r.emit(";")
    break
  case "computed":
    r.emit("[")
    renderUse(r, el.keyUse)
    r.emit("] = ")
    renderExpr(r, el.init)
    r.emit(";")
    break
  }
}

function renderUse(r, use) {
  if (use.form === "member") {
    r.emit(use.obj + ".")
    r.emitName(use)
  } else {
    r.emit("(")
    r.emitName(use)
    r.emit(" in " + use.obj + ")")
  }
}

function renderExpr(r, expr) {
  switch (expr.e) {
  case "use":
    renderUse(r, expr.use)
    break
  case "class":
    renderClass(r, expr.env)
    break
  case "arrow":
    r.emit("(() => ")
    renderStmtList(r, expr.body)
    r.emit(")")
    break
  case "comma":
    r.emit("(")
    expr.items.forEach((item, i) => {
      if (i) r.emit(", ")
      renderExpr(r, item)
    })
    r.emit(")")
    break
  case "lit":
    r.emit(expr.text)
    break
  }
}

// All private-name ranges recorded on the model, sorted by position.
// Each record carries `owner` so transforms and error sites can be
// tracked back to the model object.
function collectNames(prog) {
  const recs = []
  for (const decl of prog.decls)
    recs.push({name: decl.name, start: decl.range.start, end: decl.range.end, kind: "decl", owner: decl})
  for (const use of prog.uses)
    recs.push({name: use.name, start: use.range.start, end: use.range.end, kind: "use", owner: use})
  recs.sort((a, b) => a.start - b.start || a.end - b.end)
  return recs
}

// ---------------------------------------------------------------------
// Transforms. `rename` and `reorder` operate on the model (the program
// is re-rendered afterwards, which recomputes every range); `whitespace`
// is a textual transform that shifts ranges explicitly.

// Consistent renaming of one binding group: a declaration and exactly
// the uses that resolve to it. Shadowed inner declarations and their
// uses are left alone.
function transformRename(prog, rng) {
  const envsWithDecls = prog.envs.filter(env => env.decls.length)
  if (!envsWithDecls.length) return false
  const env = pick(rng, envsWithDecls)
  const name = pick(rng, env.declaredNames())
  const groupUses = prog.uses.filter(u => u.name === name && resolveUse(u) === env)
  const taken = new Set([...prog.decls.map(d => d.name), ...prog.uses.map(u => u.name)])
  let fresh = name + "r"
  while (taken.has(fresh)) fresh += "r"
  for (const decl of env.decls) if (decl.name === name) decl.name = fresh
  for (const use of groupUses) use.name = fresh
  return true
}

// Forward-declaration reorder: move the first declaring element of some
// class body behind the other elements, so uses precede declarations.
// Legality is order-independent, so accepted programs stay accepted.
function transformReorder(prog, rng) {
  const candidates = prog.envs.filter(env => {
    const i = env.elements.findIndex(el => el.decl)
    return i >= 0 && i < env.elements.length - 1
  })
  if (!candidates.length) return false
  const env = pick(rng, candidates)
  const i = env.elements.findIndex(el => el.decl)
  const [el] = env.elements.splice(i, 1)
  env.elements.push(el)
  return true
}

// Whitespace/comment insertion at recorded safe points; ranges are
// recomputed by shifting them past the inserted text.
function applyWhitespaceTransform(code, recs, points, rng) {
  const pads = [" ", "\n", " /*c*/ ", "\n  ", "\t"]
  const insertions = []
  const count = 1 + randInt(rng, 3)
  for (let i = 0; i < count; i++)
    insertions.push({pos: pick(rng, points), text: pick(rng, pads)})
  insertions.sort((a, b) => a.pos - b.pos)
  const newRecs = recs.map(rec => {
    let startShift = 0
    let endShift = 0
    for (const ins of insertions) {
      if (ins.pos <= rec.start) startShift += ins.text.length
      if (ins.pos < rec.end) endShift += ins.text.length
    }
    return {...rec, start: rec.start + startShift, end: rec.end + endShift}
  })
  let out = ""
  let last = 0
  for (const ins of insertions) {
    out += code.slice(last, ins.pos) + ins.text
    last = ins.pos
  }
  out += code.slice(last)
  return {code: out, recs: newRecs}
}

// ---------------------------------------------------------------------
// Case builders: construct the model relations first, render later.

// Adds 0-2 extra self-contained declaration/use groups to a class.
function addFillerDecls(prog, env, rng, chance) {
  const count = rng() < chance ? 1 + randInt(rng, 2) : 0
  for (let i = 0; i < count; i++) {
    const name = pickFreshDeclName(prog, env, rng)
    if (rng() < 0.5) {
      addField(prog, env, name, {isStatic: rng() < 0.25, init: lit("1")})
    } else {
      addMethod(prog, env, name, {body: []})
    }
    if (rng() < 0.6)
      addPublicMethod(prog, env, {body: [useStmt(prog, env, rng, name)]})
  }
}

// Builds a nesting chain of `depth` classes (root included), each held
// in a public field of its parent. Returns the chain of envs.
function buildChain(prog, rng, depth) {
  const chain = [prog.newEnv(null, "C0")]
  for (let i = 1; i < depth; i++) {
    const inner = prog.newEnv(chain[i - 1], `C${i}`)
    addPublicField(prog, chain[i - 1], {e: "class", env: inner})
    chain.push(inner)
  }
  return chain
}

const BUILDERS = {
  // Uses may precede their declarations inside a class body.
  "forward"(prog, rng) {
    const env = prog.newEnv(null, "C0")
    const name = pickFreshDeclName(prog, env, rng)
    addPublicMethod(prog, env, {body: [useStmt(prog, env, rng, name)]})
    if (rng() < 0.5)
      addStaticBlock(prog, env, [useStmt(prog, env, rng, name, {allowReturn: false})])
    if (rng() < 0.4)
      addComputedField(prog, env, memberUse(prog, env, rng, name), lit("1"))
    // The declaration comes after the uses, on purpose.
    addField(prog, env, name, {init: lit("1")})
    if (rng() < 0.5)
      addPublicMethod(prog, env, {body: [useStmt(prog, env, rng, name)]})
    prog.topLevel.push({s: "classDecl", env})
  },

  // A nested class reaches out to an ancestor's private declaration.
  "outer-ref"(prog, rng) {
    const depth = 2 + randInt(rng, 2)
    const chain = buildChain(prog, rng, depth)
    const root = chain[0]
    const name = pickFreshDeclName(prog, root, rng)
    addField(prog, root, name, {init: lit("1")})
    const inner = chain[chain.length - 1]
    addPublicMethod(prog, inner, {body: [useStmt(prog, inner, rng, name)]})
    if (rng() < 0.5)
      addStaticBlock(prog, inner, [useStmt(prog, inner, rng, name, {allowReturn: false})])
    addFillerDecls(prog, inner, rng, 0.5)
    prog.topLevel.push({s: "classDecl", env: root})
  },

  // An inner declaration shadows the outer one of the same name.
  "shadow"(prog, rng) {
    const chain = buildChain(prog, rng, 2)
    const [outer, inner] = chain
    const name = pickFreshDeclName(prog, outer, rng)
    addField(prog, outer, name, {init: lit("1")})
    addPublicMethod(prog, outer, {body: [useStmt(prog, outer, rng, name)]})
    addField(prog, inner, name, {init: lit("2")})
    addPublicMethod(prog, inner, {body: [useStmt(prog, inner, rng, name)]})
    if (rng() < 0.5) {
      // A third level sees the inner declaration, not the outer one.
      const deep = prog.newEnv(inner, "C2")
      addPublicField(prog, inner, {e: "class", env: deep})
      addPublicMethod(prog, deep, {body: [useStmt(prog, deep, rng, name)]})
    }
    prog.topLevel.push({s: "classDecl", env: outer})
  },

  // Legal getter/setter pairs (same name, same static-ness, one each).
  "accessor-pair"(prog, rng) {
    const env = prog.newEnv(null, "C0")
    const name = pickFreshDeclName(prog, env, rng)
    const isStatic = rng() < 0.4
    const getBody = [{s: "return", e: lit("1")}]
    const setBody = []
    if (rng() < 0.5) {
      addAccessor(prog, env, name, "get", {isStatic, body: getBody})
      addAccessor(prog, env, name, "set", {isStatic, body: setBody})
    } else {
      addAccessor(prog, env, name, "set", {isStatic, body: setBody})
      addAccessor(prog, env, name, "get", {isStatic, body: getBody})
    }
    addPublicMethod(prog, env, {body: [useStmt(prog, env, rng, name)]})
    if (rng() < 0.5) {
      // A second, independent pair with the other static-ness.
      const other = pickFreshDeclName(prog, env, rng)
      addAccessor(prog, env, other, "get", {isStatic: !isStatic, body: getBody})
      addAccessor(prog, env, other, "set", {isStatic: !isStatic, body: setBody})
    }
    addFillerDecls(prog, env, rng, 0.4)
    prog.topLevel.push({s: "classDecl", env})
  },

  // Heritage (extends) expressions see the environment entered before
  // the class body: uses there bind to outer declarations.
  "heritage-ok"(prog, rng) {
    const outer = prog.newEnv(null, "C0")
    const name = pickFreshDeclName(prog, outer, rng)
    addField(prog, outer, name, {init: lit("1")})
    const inner = prog.newEnv(outer, "C1")
    // The heritage use belongs to the outer environment.
    const heritageUse = prog.newUse(outer, name, "member", pick(rng, ["base", "obj"]))
    const variant = rng()
    if (variant < 0.35) {
      inner.heritage = useExpr(heritageUse)
    } else if (variant < 0.7) {
      inner.heritage = {e: "comma", items: [useExpr(heritageUse), lit("Object")]}
    } else {
      // A class nested in the heritage belongs to the outer environment
      // as well; give it its own self-contained declaration.
      const deep = prog.newEnv(outer, "C2")
      const deepName = pickFreshDeclName(prog, deep, rng)
      addField(prog, deep, deepName, {init: lit("1")})
      addPublicMethod(prog, deep, {body: [useStmt(prog, deep, rng, deepName)]})
      inner.heritage = {e: "comma", items: [useExpr(heritageUse), {e: "class", env: deep}]}
    }
    // The inner class may declare and use its own names.
    const own = pickFreshDeclName(prog, inner, rng)
    addField(prog, inner, own, {init: lit("1")})
    addPublicMethod(prog, inner, {body: [useStmt(prog, inner, rng, own)]})
    addPublicField(prog, outer, {e: "class", env: inner})
    prog.topLevel.push({s: "classDecl", env: outer})
  },

  // Chains of nested classes with uses bound at various depths.
  "deep-nest"(prog, rng) {
    const depth = 3 + randInt(rng, 2)
    const chain = buildChain(prog, rng, depth)
    const root = chain[0]
    const name = pickFreshDeclName(prog, root, rng)
    addField(prog, root, name, {init: lit("1")})
    // Uses of the root name at every level below the root.
    for (let i = 1; i < depth; i++) {
      if (rng() < 0.7)
        addPublicMethod(prog, chain[i], {body: [useStmt(prog, chain[i], rng, name)]})
    }
    // A local declaration in the deepest class, used there.
    const deepest = chain[depth - 1]
    const own = pickFreshDeclName(prog, deepest, rng)
    addField(prog, deepest, own, {init: lit("1")})
    addPublicMethod(prog, deepest, {body: [useStmt(prog, deepest, rng, own)]})
    prog.topLevel.push({s: "classDecl", env: root})
  },

  // A single use of a name that no environment in the chain declares.
  "undeclared-use"(prog, rng) {
    const depth = 1 + randInt(rng, 3)
    const chain = buildChain(prog, rng, depth)
    for (const env of chain) addFillerDecls(prog, env, rng, 0.7)
    const env = pick(rng, chain)
    const missing = prog.freshUndeclared()
    if (rng() < 0.5) {
      addPublicMethod(prog, env, {body: [useStmt(prog, env, rng, missing)]})
    } else {
      addStaticBlock(prog, env, [useStmt(prog, env, rng, missing, {allowReturn: false})])
    }
    prog.topLevel.push({s: "classDecl", env: chain[0]})
  },

  // A heritage use that the class's own body declaration cannot fix:
  // the extends expression belongs to the outer environment.
  "extends-env"(prog, rng) {
    const outer = prog.newEnv(null, "C0")
    if (rng() < 0.5) addFillerDecls(prog, outer, rng, 1)
    const inner = prog.newEnv(outer, "C1")
    // Fresh for the outer environment: the heritage use (which belongs
    // to the outer environment) must stay undeclared there.
    const name = pickFreshDeclName(prog, outer, rng)
    // The inner class declares the name in its own body...
    addField(prog, inner, name, {init: lit("1")})
    addPublicMethod(prog, inner, {body: [useStmt(prog, inner, rng, name)]})
    // ...but the heritage use belongs to the outer environment, which
    // never declares it.
    const heritageUse = prog.newUse(outer, name, "member", pick(rng, ["base", "obj"]))
    inner.heritage = rng() < 0.5
      ? useExpr(heritageUse)
      : {e: "comma", items: [useExpr(heritageUse), lit("Object")]}
    addPublicField(prog, outer, {e: "class", env: inner})
    prog.topLevel.push({s: "classDecl", env: outer})
  },

  // A field/method duplicates a same-name private declaration.
  "conflict-dup"(prog, rng) {
    const env = prog.newEnv(null, "C0")
    const name = pickFreshDeclName(prog, env, rng)
    const kinds = pick(rng, [
      ["field", "field"],
      ["method", "method"],
      ["field", "method"],
      ["method", "field"],
      ["staticField", "field"],
      ["field", "staticField"]
    ])
    for (const kind of kinds) {
      if (kind === "field") addField(prog, env, name, {init: lit("1")})
      else if (kind === "staticField") addField(prog, env, name, {isStatic: true, init: lit("1")})
      else addMethod(prog, env, name, {body: []})
    }
    addFillerDecls(prog, env, rng, 0.5)
    prog.topLevel.push({s: "classDecl", env})
  },

  // Illegal accessor combinations: repeated getters, static/instance
  // crosses, or accessors mixed with fields.
  "conflict-accessor"(prog, rng) {
    const env = prog.newEnv(null, "C0")
    const name = pickFreshDeclName(prog, env, rng)
    const variant = pick(rng, [
      [["get", false], ["get", false]],
      [["set", false], ["set", false]],
      [["get", true], ["set", false]],
      [["get", false], ["set", true]],
      [["field", false], ["get", false]],
      [["get", false], ["field", false]],
      [["get", false], ["set", false], ["get", false]],
      [["get", false], ["set", false], ["set", false]]
    ])
    for (const [kind, isStatic] of variant) {
      if (kind === "field") addField(prog, env, name, {init: lit("1")})
      else addAccessor(prog, env, name, kind, {isStatic, body: kind === "get" ? [{s: "return", e: lit("1")}] : []})
    }
    addFillerDecls(prog, env, rng, 0.5)
    prog.topLevel.push({s: "classDecl", env})
  }
}

export const CATEGORIES = [
  "forward",
  "outer-ref",
  "shadow",
  "accessor-pair",
  "heritage-ok",
  "deep-nest",
  "undeclared-use",
  "extends-env",
  "conflict-dup",
  "conflict-accessor"
]

export const REJECT_CATEGORIES = new Set(["undeclared-use", "extends-env", "conflict-dup", "conflict-accessor"])

// ---------------------------------------------------------------------
// Case finalization: model check, transforms, render, expectations.

function pickTransformFamily(rng, expect) {
  const r = rng()
  if (expect === "accept") {
    if (r < 0.3) return {rename: false, reorder: false, whitespace: false}
    if (r < 0.5) return {rename: true, reorder: false, whitespace: false}
    if (r < 0.65) return {rename: false, reorder: true, whitespace: false}
    if (r < 0.8) return {rename: false, reorder: false, whitespace: true}
    return {rename: true, reorder: true, whitespace: true}
  }
  if (r < 0.5) return {rename: false, reorder: false, whitespace: false}
  if (r < 0.75) return {rename: true, reorder: false, whitespace: false}
  return {rename: false, reorder: false, whitespace: true}
}

function snapshotModel(prog) {
  return {
    envs: prog.envs.map(env => ({
      id: env.id,
      parent: env.parent ? env.parent.id : null,
      className: env.className,
      decls: env.decls.map(d => ({name: d.name, kind: d.kind, static: d.static}))
    })),
    uses: prog.uses.map(u => ({name: u.name, form: u.form, env: u.env.id}))
  }
}

function finalizeCase(prog, {id = -1, category = "manual", rng = null, allowTransforms = false} = {}) {
  const problems = checkModel(prog)
  const expect = problems.length ? "reject" : "accept"
  if (problems.length > 1)
    throw new Error(`generator bug: category ${category} produced ${problems.length} problems`)
  const applied = []
  let whitespacePlanned = false
  if (allowTransforms && rng) {
    const family = pickTransformFamily(rng, expect)
    if (family.rename && transformRename(prog, rng)) applied.push("rename")
    if (family.reorder && transformReorder(prog, rng)) applied.push("reorder")
    whitespacePlanned = family.whitespace
  }
  const {code, points} = renderProgram(prog)
  let recs = collectNames(prog)
  let finalCode = code
  if (allowTransforms && whitespacePlanned) {
    const shifted = applyWhitespaceTransform(code, recs, points, rng)
    finalCode = shifted.code
    recs = shifted.recs
    applied.push("whitespace")
  }
  const names = recs.map(({name, start, end, kind}) => ({name, start, end, kind}))
  let errorName = null
  if (problems.length) {
    const rec = recs.find(r => r.owner === problems[0].owner)
    errorName = {name: rec.name, start: rec.start, end: rec.end}
  }
  return {
    id,
    category,
    expect,
    code: finalCode,
    names,
    errorName,
    transform: applied.length ? applied.join("+") : "none",
    model: snapshotModel(prog)
  }
}

export function generateCase(rng, category, id) {
  const prog = new Prog()
  BUILDERS[category](prog, rng)
  return finalizeCase(prog, {id, category, rng, allowTransforms: true})
}

// Builds one small, auditable case directly through the model. Used for
// the mutation-testing detection scenarios.
export function buildModelCase(category, build) {
  const prog = new Prog()
  build(prog)
  return finalizeCase(prog, {category})
}

// The small auditable cases used by the mutation tests, all built
// through the same model and renderer as the generated cases.
export function buildMutantDetectionCases() {
  // Accepted by the original parser and by every mutant.
  const sanity = buildModelCase("sanity", prog => {
    const env = prog.newEnv(null, "C0")
    addField(prog, env, "a", {init: lit("1")})
    addMethod(prog, env, "m", {body: [plainUseStmt(prog, env, "a")]})
    prog.topLevel.push({s: "classDecl", env})
  })
  // Same, plus a plain (private-name-free) heritage clause.
  const sanityHeritage = buildModelCase("sanity-heritage", prog => {
    const env = prog.newEnv(null, "C0")
    env.heritage = lit("Base")
    addField(prog, env, "a", {init: lit("1")})
    prog.topLevel.push({s: "classDecl", env})
  })
  // An undeclared use inside a nested class: only propagation to the
  // outer class environment can surface this error.
  const outerLookup = buildModelCase("outer-lookup", prog => {
    const outer = prog.newEnv(null, "C0")
    const inner = prog.newEnv(outer, "C1")
    addMethod(prog, inner, "m", {body: [plainUseStmt(prog, inner, "u0")]})
    addPublicField(prog, outer, {e: "class", env: inner})
    prog.topLevel.push({s: "classDecl", env: outer})
  })
  // A static getter and an instance setter of the same name must not pair.
  const accessorStatic = buildModelCase("accessor-static", prog => {
    const env = prog.newEnv(null, "C0")
    addAccessor(prog, env, "p", "get", {isStatic: true, body: [{s: "return", e: lit("1")}]})
    addAccessor(prog, env, "p", "set", {isStatic: false})
    prog.topLevel.push({s: "classDecl", env})
  })
  // A heritage use that only the class's own body declares: the extends
  // expression belongs to the outer environment, so this is undeclared.
  const extendsBoundary = buildModelCase("extends-boundary", prog => {
    const outer = prog.newEnv(null, "C0")
    const inner = prog.newEnv(outer, "C1")
    inner.heritage = useExpr(prog.newUse(outer, "x", "member", "base"))
    addField(prog, inner, "x", {init: lit("1")})
    addPublicField(prog, outer, {e: "class", env: inner})
    prog.topLevel.push({s: "classDecl", env: outer})
  })
  return {sanity, sanityHeritage, outerLookup, accessorStatic, extendsBoundary}
}

// ---------------------------------------------------------------------
// Private-name scanner: derives expected names/ranges straight from
// source text (used for the fixed readable cases and as a renderer
// self-check). Skips strings and comments; cooks unicode escapes.

export function scanPrivateNames(code) {
  const out = []
  const n = code.length
  let i = 0
  while (i < n) {
    const c = code[i]
    if (c === "'" || c === '"' || c === "`") {
      i++
      while (i < n && code[i] !== c) {
        if (code[i] === "\\") i++
        i++
      }
      i++
    } else if (c === "/" && code[i + 1] === "/") {
      while (i < n && code[i] !== "\n") i++
    } else if (c === "/" && code[i + 1] === "*") {
      i += 2
      while (i < n && !(code[i] === "*" && code[i + 1] === "/")) i++
      i += 2
    } else if (c === "#") {
      const start = i
      i++
      let name = ""
      while (i < n) {
        const ch = code[i]
        if (ch === "\\" && code[i + 1] === "u") {
          let j = i + 2
          let cp
          if (code[j] === "{") {
            const close = code.indexOf("}", j)
            cp = parseInt(code.slice(j + 1, close), 16)
            j = close + 1
          } else {
            cp = parseInt(code.slice(j, j + 4), 16)
            j += 4
          }
          name += String.fromCodePoint(cp)
          i = j
        } else if (/[$\w]/.test(ch) || ch.charCodeAt(0) > 127) {
          name += ch
          i++
        } else break
      }
      out.push({name, start, end: i})
    } else i++
  }
  return out
}

// ---------------------------------------------------------------------
// Parser-facing checking.

function collectPrivateIdentifiers(ast) {
  const out = []
  const visit = node => {
    if (!node || typeof node !== "object") return
    if (Array.isArray(node)) {
      for (const item of node) visit(item)
      return
    }
    if (typeof node.type !== "string") return
    if (node.type === "PrivateIdentifier") out.push({name: node.name, start: node.start, end: node.end})
    for (const key of Object.keys(node)) {
      if (key === "loc" || key === "range" || key === "start" || key === "end") continue
      visit(node[key])
    }
  }
  visit(ast)
  out.sort((a, b) => a.start - b.start || a.end - b.end)
  return out
}

// Compares one case against the parser. Accepted programs must produce
// exactly the modeled PrivateIdentifier names and UTF-16 ranges;
// rejected programs must fail with a SyntaxError whose pos points at
// the controlled error's name.
export function checkCase(parse, kase) {
  let ast = null
  let err = null
  try {
    ast = parse(kase.code, PARSE_OPTIONS)
  } catch (e) {
    err = e
  }
  if (kase.expect === "accept") {
    if (err)
      return {ok: false, kind: "accept/reject", detail: `expected accept but parser rejected at pos ${err.pos}: ${err.message}`}
    const actual = collectPrivateIdentifiers(ast)
    const expected = [...kase.names].sort((a, b) => a.start - b.start || a.end - b.end)
    if (actual.length !== expected.length)
      return {ok: false, kind: "name-set", detail: `expected ${expected.length} private identifiers, parser produced ${actual.length}`, actual, expected}
    for (let i = 0; i < actual.length; i++) {
      const a = actual[i]
      const e = expected[i]
      if (a.name !== e.name || a.start !== e.start || a.end !== e.end)
        return {ok: false, kind: "name-position", detail: `private identifier ${i} mismatch: expected #${e.name} at ${e.start}..${e.end}, parser gave #${a.name} at ${a.start}..${a.end}`, actual, expected}
    }
    return {ok: true}
  }
  if (!err)
    return {ok: false, kind: "accept/reject", detail: "expected reject but parser accepted"}
  if (!(err instanceof SyntaxError))
    return {ok: false, kind: "error-type", detail: `expected SyntaxError, got ${err && err.constructor && err.constructor.name}: ${err && err.message}`}
  if (typeof err.pos !== "number")
    return {ok: false, kind: "error-type", detail: `SyntaxError without pos: ${err.message}`}
  const target = kase.errorName
  if (err.pos < target.start || err.pos >= target.end)
    return {ok: false, kind: "name-position", detail: `error pos ${err.pos} does not point at controlled name #${target.name} (${target.start}..${target.end}): ${err.message}`}
  return {ok: true}
}

// ---------------------------------------------------------------------
// Batch runner.

export function runBatch({seed, cases, parse, onFailure}) {
  const rng = makeRng(seed)
  const counts = Object.create(null)
  const bump = key => { counts[key] = (counts[key] || 0) + 1 }
  for (let i = 0; i < cases; i++) {
    const category = CATEGORIES[i % CATEGORIES.length]
    const kase = generateCase(rng, category, i)
    bump(`case:${kase.expect}:${kase.category}`)
    bump(`transform:${kase.transform}`)
    // Renderer self-check: an independent scan of the emitted text must
    // reproduce the recorded names and ranges.
    const scanned = scanPrivateNames(kase.code)
    const recorded = kase.names
    if (scanned.length !== recorded.length ||
        scanned.some((s, j) => s.name !== recorded[j].name || s.start !== recorded[j].start || s.end !== recorded[j].end)) {
      const failure = {seed, caseIndex: i, kase, result: {ok: false, kind: "renderer", detail: "renderer ranges disagree with source scan", scanned, recorded}}
      if (onFailure) onFailure(failure)
      return {counts, failures: [failure]}
    }
    const result = checkCase(parse, kase)
    if (!result.ok) {
      const failure = {seed, caseIndex: i, kase, result}
      if (onFailure) onFailure(failure)
      return {counts, failures: [failure]}
    }
  }
  return {counts, failures: []}
}

// Persists everything needed to reproduce a divergence: source, model,
// seed and the first error range.
export function saveFailure(failure) {
  const file = join(tmpdir(), `acorn-private-scope-model-failure-seed${failure.seed}-case${failure.caseIndex}.json`)
  const firstErrorRange = failure.kase.errorName ||
    (failure.result.expected && failure.result.expected[0]) ||
    (failure.result.recorded && failure.result.recorded[0]) ||
    null
  writeFileSync(file, JSON.stringify({
    seed: failure.seed,
    caseIndex: failure.caseIndex,
    category: failure.kase.category,
    transform: failure.kase.transform,
    code: failure.kase.code,
    model: failure.kase.model,
    names: failure.kase.names,
    errorName: failure.kase.errorName,
    firstErrorRange,
    result: {kind: failure.result.kind, detail: failure.result.detail}
  }, null, 2))
  return file
}

// ---------------------------------------------------------------------
// CLI: node test/private-scope-model.mjs --seed 1320 --cases 160

async function main(argv) {
  let seed = 1320
  let cases = 160
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--seed") seed = Number(argv[++i])
    else if (argv[i] === "--cases") cases = Number(argv[++i])
    else {
      console.error(`unknown argument: ${argv[i]}`)
      process.exit(2)
    }
  }
  if (!Number.isInteger(seed) || !Number.isInteger(cases) || cases < 1) {
    console.error("usage: node test/private-scope-model.mjs --seed <int> --cases <int>=1..")
    process.exit(2)
  }
  const {parse} = await loadParser()
  let saved = null
  const {counts, failures} = runBatch({
    seed,
    cases,
    parse,
    onFailure(failure) { saved = saveFailure(failure) }
  })
  for (const key of Object.keys(counts).sort())
    if (counts[key] > 0) console.log(`${key} ${counts[key]}`)
  if (failures.length) {
    const failure = failures[0]
    console.error(`divergence at case ${failure.caseIndex} (${failure.kase.category}, transform ${failure.kase.transform}): ${failure.result.detail}`)
    console.error(`code: ${failure.kase.code}`)
    console.error(`saved to ${saved}`)
    process.exit(1)
  }
  console.log(`private-scope-model ok seed=${seed} cases=${cases}`)
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href)
  await main(process.argv.slice(2))
