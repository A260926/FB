Read-only final WebAssembly exception handling

References (format/semantics, no runtime dependency):
https://github.com/WebAssembly/exception-handling/blob/main/proposals/exception-handling/Exceptions.md
https://github.com/WebAssembly/exception-handling/blob/main/interpreter/valid/valid.ml
The proposal's try_table.wast includes nested handlers, different tags with
identical signatures, and catches with duplicate or shadowed alternatives.

Support samples are inert: ordinary.wasm is one empty-signature function with
nop; exception.wasm adds one empty-signature tag and one block containing a
try_table catch tag 0 label 0, a throw 0, and a nop after the outer block.
The tag has no payload. A branch from the catch continues at that final nop.
The samples package generates the same fixed bytes; baseline checks equality.
No WebAssembly engine is included or used, including during example execution.

Binary declarations: tag section = 13, import/export tag kind = 4; tag_type is
attribute byte zero and a type index. Tag declarations and function declarations
have independent imported prefixes. Equal tag signature types do not identify
the same declared tag. This analysis uses syntactic module tag identity and
performs no resolution of possible host aliases between imported tags.

Instructions: try_table = 0x1f + blocktype + catch vector;
throw = 0x08 + tag index; throw_ref = 0x0a with no immediate.
Catch kinds 0/1 take tag+label; 2/3 take label only, all unsigned LEB32.
Block types also permit signed LEB33 nonnegative type indices.
Catch labels are resolved in the OUTER label context, before adding try_table's
own body label. For example, check_catch uses c, while the body is checked in c'
in interpreter/valid/valid.ml. The body still adds a nesting level for end.

Keep this repository's PCs: CodeEntry.CodeSectionPtr and Expression.PC are
relative to the code section payload, including the vector/body-size fields.
Expression.PCNext is exclusive. Loop branch continuations begin at the first
body instruction; block/if continuation follows its matching end. The function
exit is explicit rather than a made-up instruction. This is a syntactic,
path-insensitive analysis: all decoded sites are considered, including sites
following an unconditional transfer. No operand value or reachability claim.
A function summary is its escaping declared-tag identities plus a foreign
possibility. Catch clauses filter identities in their encoded order. A declared
callee contributes its own escaping summary; an imported/indirect call or
throw_ref contributes every module tag plus foreign. A trap is not an exception.

Target new implementation and tests are absent. The target runner compiles the
current wasmfile package and example, then runs the three named regression tests
with taskcheck.Run/Check. Missing files, zero checks, skipping or early process
exit cannot emit the required group completion records. Original expression
and encoding tests use testify, so this standard-library runner does not run
those dependency-based tests; ordinary decode/PC observations remain enabled.
