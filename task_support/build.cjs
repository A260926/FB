'use strict';
// Frozen TypeScript 5.7.3 transpiler; no registry/toolchain access at task time.
const fs=require('node:fs'),path=require('node:path');
const ts=require('./vendor/typescript/lib/typescript.js');
fs.rmSync('dist',{recursive:true,force:true});
function walk(dir) {
 for(const e of fs.readdirSync(dir,{withFileTypes:true})) {
  const p=path.join(dir,e.name);
  if(e.isDirectory()) walk(p);
  else if(p.endsWith('.ts')&&!p.endsWith('.d.ts')) {
   const out=path.join('dist',path.relative('src',p).replace(/\.ts$/,'.js'));
   fs.mkdirSync(path.dirname(out),{recursive:true});
   fs.writeFileSync(out,ts.transpileModule(fs.readFileSync(p,'utf8'),{fileName:p,compilerOptions:{module:ts.ModuleKind.ESNext,target:ts.ScriptTarget.ES2022,esModuleInterop:true}}).outputText);
  }
 }
}
walk('src');console.log('BUILD_OK source=src output=dist');
