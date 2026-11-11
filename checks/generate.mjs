import { readdir, readFile, writeFile } from 'node:fs/promises';
import { preprocess } from 'preprocess';
async function walk(dir) {
 for (const e of await readdir(dir, {withFileTypes:true})) {
  const p = `${dir}/${e.name}`;
  if(e.isDirectory()) await walk(p);
  else if(p.endsWith('.template.js')) for(const indirect of [false,true]) {
   const out = p.replace('.template.js', indirect ? '_indirect.generated.js' : '.generated.js');
   await writeFile(out,preprocess(await readFile(p,'utf8'),{INDIRECT:indirect,INDIRECT_STRING:indirect?'_indirect':''},{type:'js'}));
  }
 }
}
await walk('src');
console.log('TEMPLATES_READY');
