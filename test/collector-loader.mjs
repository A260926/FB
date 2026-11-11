import {existsSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
// Resolve the source tree's emitted .js specifiers without installing a compiler.
export async function resolve(specifier, context, nextResolve) {
  if (specifier.endsWith('.js') && context.parentURL?.includes('/source/')) {
    const url = new URL(specifier.slice(0, -3) + '.ts', context.parentURL);
    if (existsSync(fileURLToPath(url))) return nextResolve(url.href, context);
  }
  return nextResolve(specifier, context);
}
