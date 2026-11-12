// Loads the real acorn parser from acorn/src without requiring the
// repository's dev dependencies or a build step. The source tree is
// copied into a fresh temporary ESM directory first, so callers may
// freely patch the copy (mutation testing) without ever touching the
// original sources, and every copy gets its own module registry entry.

import {cpSync, mkdtempSync} from "node:fs"
import {tmpdir} from "node:os"
import {join, dirname} from "node:path"
import {fileURLToPath, pathToFileURL} from "node:url"

const here = dirname(fileURLToPath(import.meta.url))

// The actual parser sources inside this repository (already an ESM
// package thanks to acorn/src/package.json).
export const parserSourceDir = join(here, "..", "acorn", "src")

// Copies acorn/src into targetDir (which must not exist yet or be empty)
// and returns targetDir.
export function copyParserSource(targetDir) {
  cpSync(parserSourceDir, targetDir, {recursive: true})
  return targetDir
}

// Creates a fresh temporary directory holding a copy of acorn/src and
// returns its path. The original sources are never modified.
export function makeParserCopy(prefix = "acorn-src-") {
  const dir = mkdtempSync(join(tmpdir(), prefix))
  copyParserSource(dir)
  return dir
}

// Imports the parser from a directory previously produced by
// makeParserCopy/copyParserSource and returns the module namespace.
// Throws when the copy cannot be loaded as a parser.
export async function importParser(dir) {
  const mod = await import(pathToFileURL(join(dir, "index.js")).href)
  if (!mod || typeof mod.parse !== "function")
    throw new Error(`parser module at ${dir} failed to load: no parse export`)
  return mod
}

// Convenience: copy acorn/src into a temporary ESM directory, load it,
// and return {dir, acorn, parse}.
export async function loadParser() {
  const dir = makeParserCopy()
  const acorn = await importParser(dir)
  return {dir, acorn, parse: acorn.parse}
}
