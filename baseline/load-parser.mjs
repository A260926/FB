import {mkdtemp, cp, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {pathToFileURL} from 'node:url';
export async function loadParser(sourceRoot=process.cwd()) {
 const dir=await mkdtemp(join(tmpdir(),'acorn-private-'));
 try {
  await cp(join(resolve(sourceRoot),'acorn/src'),join(dir,'src'),{recursive:true});
  await writeFile(join(dir,'package.json'),'{"type":"module"}\n');
  const parser=await import(pathToFileURL(join(dir,'src/index.js')).href);
  return {parser,dispose:()=>rm(dir,{recursive:true,force:true})};
 }catch(error){await rm(dir,{recursive:true,force:true});throw error;}
}
