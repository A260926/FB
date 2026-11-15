import {registerHooks,stripTypeScriptTypes} from 'node:module';
import {existsSync,readFileSync} from 'node:fs';
import {fileURLToPath,pathToFileURL} from 'node:url';
const dep=new URL('../vendor/gl-matrix/cjs/index.js',import.meta.url).href;
registerHooks({
 resolve(spec,ctx,next){
  if(spec==='gl-matrix')return {url:dep,shortCircuit:true};
  if((spec.startsWith('.')||spec.startsWith('/'))&&ctx.parentURL){
   const url=new URL(spec,ctx.parentURL);
   if(url.protocol==='file:'){
    const p=fileURLToPath(url);
    const candidates=p.endsWith('.js')?[p,p.slice(0,-3)+'.ts']:[p,p+'.ts',p+'/index.ts'];
    for(const c of candidates)if(existsSync(c)&&!c.endsWith('/')){
     // Let native resolver distinguish files/directories; our appended TS paths are files.
     if(c.endsWith('.ts'))return {url:pathToFileURL(c).href,shortCircuit:true};
    }
   }
  }
  return next(spec,ctx);
 },
 load(url,ctx,next){
  if(url.endsWith('.ts')){
   // Upstream armature barrel imports interfaces without type; mirror TS build erasure.
   const input=readFileSync(fileURLToPath(url),'utf8').replace("import { ISkin, TTextureInfo }","import type { ISkin, TTextureInfo }").replace("import { TTypeArrayCon }","import type { TTypeArrayCon }").replace("import { TTypeArray }","import type { TTypeArray }");
   const source=stripTypeScriptTypes(input,{mode:'strip',sourceUrl:url});
   return {format:'module',source,shortCircuit:true};
  }
  return next(url,ctx);
 }
});
