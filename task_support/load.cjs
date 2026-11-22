'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path');
module.exports = function() {
  const context = vm.createContext({JSMpeg: {Decoder: {}, Now: () => 0,
    // Fixed upstream src/jsmpeg.js Fill helper, required by nonzero AC blocks.
    Fill: function(array, value) {
      if (array.fill) array.fill(value);
      else for (var i = 0; i < array.length; i++) array[i] = value;
    }}, console});
  for (const file of ['buffer.js','decoder.js','mpeg1.js'])
    vm.runInContext(fs.readFileSync(path.join(__dirname,'../src',file),'utf8'), context, {filename:file});
  return context.JSMpeg;
};
