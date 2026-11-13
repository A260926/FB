Powerful Disassembler Library For x86/AMD64
-----------

Welcome to the diStorm3 binary stream disassembler library project.

diStorm3 is really a decomposer, which means it takes an instruction and returns a binary structure which describes it rather than static text, which is great for advanced binary code analysis.

diStorm3 is super lightweight (~45KB), ultra fast and easy to use (a single API), licensed under BSD!

For a light hooking library see the https://github.com/gdabah/distormx project.

"We benchmarked five popular open-source disassembly libraries and chose diStorm3, which had the best performance (and furthermore, has complete 64-bit support).", July 2014, Quoting David Williams-King in his Thesis about Binary Shuffling.

Installing diStorm3 -
'python -m pip install distorm3'

RTFM, the wiki has plenty of info.

Prefix tracing
-
Besides the unused-prefix bitmask, `distorm_decompose_prefix_trace()` (see `include/prefix_trace.h`) explains, per prefix byte in stream order, the role it played and, when it had no effect, the reason (same-group override, REX too far from the opcode, REX.W over 0x66, mode-ignored, or unused by the instruction). A VEX prefix is one 2/3-byte record whose `pp` field reports the implied virtual mandatory selector without inventing bytes. A runnable C example and the result-structure reference live in `examples/prefix_trace/`.
