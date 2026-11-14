const Module = require('node:module')
const path = require('node:path')
const root = path.resolve(__dirname, '..') + path.sep
const resolve = Module._resolveFilename
Module._resolveFilename = function (request, parent, ...rest) {
  const result = resolve.call(this, request, parent, ...rest)
  if (path.isAbsolute(result) && (!result.startsWith(root) || result.includes(path.sep + 'node_modules' + path.sep))) {
    throw new Error(`Only source files in this tree and Node builtins are allowed: ${request}`)
  }
  return result
}
