import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
const require = createRequire(import.meta.url);
export async function resolve(specifier, context, nextResolve) {
    if (specifier === 'earcut') return nextResolve(pathToFileURL(require.resolve('earcut')).href, context);
    if (context.parentURL?.includes('/src/') && specifier.startsWith('./') && !specifier.endsWith('.js')) {
        specifier += '.js';
    }
    return nextResolve(specifier, context);
}
export async function load(url, context, nextLoad) {
    if (url.includes('/src/') && !url.includes('/node_modules/')) {
        return nextLoad(url, {...context, format:'module'});
    }
    return nextLoad(url, context);
}
