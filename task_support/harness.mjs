// Host adapter only: the original production modules perform all analysis.
globalThis.radare2 = null;
// Same host constants as c/r2dec.c; no analysis logic is substituted.
globalThis.Limits = {UT64_MAX:0xffffffffffffffffn,UT32_MAX:0xffffffffn,UT16_MAX:0xffffn,ST64_MAX:0x7fffffffffffffffn,ST32_MAX:0x7fffffffn,ST16_MAX:0x7fffn};
let shared = {};
globalThis.Global = () => shared;
const [{default: Core}, {default: X86}, {default: Context}, {default: Printer}, {default: TestSuite}] = await Promise.all([
 import('../js/libdec/core.js'), import('../js/libdec/arch/x86.js'),
 import('../js/libdec/context.js'), import('../js/libdec/printer.js'), import('../js/libdec/testsuite.js')
]);
export function decompile(issue) {
 globalThis.unit = {file:'synthetic.json',raw:JSON.stringify(issue)};
 const test = new TestSuite();
 shared = {evars:test.evars,argdb:test.data.argdb,warning:(s)=>{throw new Error(s);}};
 shared.evars.honor.assembly=false;shared.evars.extra.allfunctions=true;
 shared.printer = new Printer();shared.context = new Context();
 const session = new Core.session(test.data,X86);
 const ac = X86.context(test.data);
 Core.analysis.pre(session,X86,ac);
 Core.decompile(session,X86,ac);
 Core.analysis.post(session,X86,ac);
 Core.print(session);
 if(shared.context.errors.length) throw new Error(shared.context.errors.join('\n'));
 return {session,lines:shared.context.lines.map(x=>({str:x.str,offset:x.offset?.toString(16)??null})),text:shared.context.lines.map(x=>x.str).join('\n')};
}
