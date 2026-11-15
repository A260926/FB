import { readFile } from 'node:fs/promises';
export async function load(url, context, nextLoad) {
 if (url.endsWith('.glsl')) return { format:'module', source:'export default '+JSON.stringify(await readFile(new URL(url),'utf8'))+';', shortCircuit:true };
 return nextLoad(url, context);
}
