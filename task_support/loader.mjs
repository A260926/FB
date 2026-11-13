import {existsSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
export async function resolve(specifier,context,nextResolve){
 if(specifier.startsWith('.')&&specifier.endsWith('.js')&&context.parentURL){
  const target=new URL(specifier.slice(0,-3)+'.ts',context.parentURL);
  if(target.protocol==='file:'&&existsSync(fileURLToPath(target)))return {url:target.href,shortCircuit:true};
 }
 return nextResolve(specifier,context);
}
