# Third-party notices

MemPDB is original software licensed under the MIT License (see [`LICENSE`](LICENSE)).
This file documents third-party material that informed or was referenced while
implementing the parser.

## LLVM Project - PDB / CodeView layout reference

MemPDB's on-disk structure definitions (see `src/PDBFormat.hpp`) describe the
publicly documented Microsoft MSF / PDB / CodeView binary formats. Layouts were
cross-checked against headers in the [LLVM Project](https://github.com/llvm/llvm-project)
(`llvm/DebugInfo/PDB`, `llvm/DebugInfo/CodeView`).

LLVM is licensed under the **Apache License v2.0 with LLVM Exceptions**:

- License text: https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT
- SPDX: `Apache-2.0 WITH LLVM-exception`

That license **allows** reuse, modification, and redistribution of LLVM code and
derived works, including in MIT-licensed projects, provided Apache-2.0
redistribution conditions are met for any LLVM-derived portions (retain notices;
include a copy of the Apache-2.0 license / this attribution for those portions).

MemPDB does **not** link against LLVM libraries. It is a separate implementation.
No LLVM runtime or binary is redistributed with this project.

See the LLVM `LICENSE.TXT` for the full Apache-2.0 terms and LLVM Exceptions.

## Microsoft PDB / CodeView format

The PDB, MSF, and CodeView formats are Microsoft formats. Format documentation
and on-disk record layouts are used here as a published binary specification.
Microsoft, Windows, and related names are trademarks of Microsoft Corporation.

## libcurl (Linux builds)

On Linux, HTTP symbol-server downloads use [libcurl](https://curl.se/libcurl/),
which is typically linked dynamically against the system package and is not
vendored in this repository. libcurl is available under a license based on MIT/X
(see https://curl.se/docs/copyright.html).
