const Module = require('node:module');
const original = Module._resolveFilename;
Module._resolveFilename = function (id, ...args) {
  if (id === '@test/test-target') return process.cwd() + '/.build/index.cjs';
  return original.call(this, id, ...args);
};
