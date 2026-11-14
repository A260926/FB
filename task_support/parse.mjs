import sax from '../vendor/sax/lib/sax.js';
import {fromParser} from '../src/main/js/doc.js';
export function parse(xml) {
 return fromParser(xml, {fatal(m){throw new Error(m);},error(m){throw new Error(m);}}, null, sax.parser(true,{xmlns:true}));
}
