import assert from 'node:assert/strict';
import {loadParser} from './load-parser.mjs';
const loaded=await loadParser();
try {
 const cases=[
  ['forward', 'class A{ m(o){return o.#x} #x; }', true],
  ['outer', 'class A{ #x; m(){return class B{m(o){return o.#x}}}}', true],
  ['shadow', 'class A{m(){return class B{m(o){return o.#x} #x}}}', true],
  ['heritage', 'class A{m(o){return class B extends (o.#x){#x}}}', false],
  ['pair', 'class A{static get #x(){return 1} static set #x(v){}}', true],
  ['conflict', 'class A{get #x(){return 1} static set #x(v){}}', false]
 ];
 for(const [name,source,accepted] of cases){
  let got=true;try {const ast=loaded.parser.parse(source,{ecmaVersion:2022,locations:true});assert.equal(ast.type,'Program');}
  catch(error){if(!(error instanceof SyntaxError))throw error;got=false;assert(Number.isInteger(error.pos));}
  assert.equal(got,accepted,name);console.log(`ACORN_PRIVATE_${name}_PASS`);
 }
 console.log('ACORN_PRIVATE_BASELINE_OK cases=6');
}finally{await loaded.dispose();}
