Go metadata scope: Go 1.20 through Go 1.26, little-endian, 64-bit, ordinary single-text Go executables.

Primary format sources (Go project, BSD-3-Clause; Go-LICENSE is preserved):
- https://github.com/golang/go/blob/go1.20/src/runtime/runtime2.go (_func)
- https://github.com/golang/go/blob/go1.20/src/runtime/symtab.go (moduledata)
- https://github.com/golang/go/blob/go1.20/src/runtime/symtab.go (inlinedCall)
- https://github.com/golang/go/blob/go1.26.4/src/runtime/runtime2.go
- https://github.com/golang/go/blob/go1.26.4/src/runtime/symtab.go
- https://github.com/golang/go/blob/go1.26.4/src/runtime/symtabinl.go

For this scope the _func prefix is 44 bytes: entryOff u32 at 0, nameOff i32 at 4, args i32 at 8, deferreturn u32 at 12, pcsp/pcfile/pcln u32 at 16/20/24, npcdata u32 at 28, cuOffset u32 at 32, startLine i32 at 36, funcID/flag/pad/nfuncdata bytes at 40/41/42/43. Following it are npcdata u32 PCDATA offsets and then nfuncdata u32 FUNCDATA offsets. PCDATA slot 2 is InlTreeIndex; FUNCDATA slot 3 is InlTree. PCDATA offset 0 means absent; FUNCDATA offset 0xffffffff means absent, while zero is a valid offset. FUNCDATA offsets are relative to moduledata.gofunc VA.

moduledata.gofunc is a pointer at byte offset 320 in these 64-bit versions. This pointer itself is not a file offset. Keep the existing layout selection and validation; in Go 1.26 epclntab shifts fields after gofunc, so older textsectmap offsets must not be substituted into the new layout.

An inlinedCall record occupies 16 bytes: funcID u8 plus 3 padding bytes, nameOff i32 at 4, parentPc i32 at 8, startLine i32 at 12. parentPc denotes an instruction offset in the physical function, not a parent record index. The line/file tables are those of the physical function, even when they describe an inlined callee. PC inputs here are instruction PCs; callers holding return addresses must normalize them before calling.

fixture/main.go is authored input. fixtures.sh compiles it for ELF/amd64, PE/amd64, Mach-O/arm64 using the local Go compiler and removes native symbol and DWARF information via -s -w. It never runs these programs. GoReSym runs on the host to read their bytes. The baseline only checks the existing physical function/file/line pipeline, not inline recovery.
