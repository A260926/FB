# ARM-disasm

This disassembler library was initially developed for the ["Black Magic" debugger](https://github.com/compuphase/Black-Magic-Probe-Book). This debugger is a front-end for GDB, with specific support for the [Black Magic Probe](https://github.com/blackmagic-debug/blackmagic), and a focus on debugging micro-controller projects. The Black Magic Probe targets ARM Cortex M0, M3, and A7 architectures, so these are also the target architectures for the ARM-disasm library. Only the 32-bit architecture is supported (32-bit ARM and Thumb/Thumb-2).

The library is written in plain C with C99 extensions.

# License

ARM-disasm is licensed under the [Apache License version 2](https://www.apache.org/licenses/LICENSE-2.0).

# Usage

The first step is to initialize the library.

    void disasm_init(ARMSTATE *state, int flags);

The first parameter contains the state of the disassembly, and it is updated after each step. The decoded instructions are read from this variable too. The `flags` parameter contains options for decoding.

* `DISASM_ADDRESS`  prefix the instruction address to the result
* `DISASM_INSTR`    prefix the binary encoding of the instruction to the result
* `DISASM_COMMENT`  add comments with symbolic information, or the value in a different base, if applicable

When done, you need to clean up the internal structures. The `disasm_cleanup` function frees all memory that was allocation during the use of the other functions.

    void disasm_cleanup(ARMSTATE *state);

On the ARM architecture, jump targets (for control flow and function calls) are relative to the instruction address (register `PC`). Therefore, to resolve the address that is really jumped to, you need to know the value of `PC`. The starting value is set with `disasm_address`. The disassembler library then updates the instruction address while disassembling.

    void disasm_address(ARMSTATE *state, uint32_t address);

If you do not disassemble the code in a single run (from the starting address to the end), but instead (for example) on a function-by-function basis, you can call `disasm_address` at the start of each run.

## Decoding a single instruction

At a low level, there are two functions for disassembling ARM instructions: one for Thumb/Thumb-2 and one for the original 32-bit ARM instructions.

    bool disasm_thumb(ARMSTATE *state, uint16_t hw, uint16_t hw2);

    bool disasm_arm(ARMSTATE *state, uint32_t w);

The reason for splitting it into two functions is that this is how an ARM processor works, too. An ARM processor (assuming it supports both instruction sets) is in either ARM mode or in Thumb mode. This "mode" is set in the low bit of the instruction address (register `PC`), but it is not as simple as that instructions at an odd address are Thumb and instructions at an even address are ARM. In fact, instructions are always at an even address; the ARM processor only uses the low bit *to check which mode it is in*, it ignores this bit when fetching instructions from memory.

For the `disasm_thumb` function, you have to pass in two (consecutive) 16-bit "half" words. A Thumb instruction can be either 16-bit (in which case the `hw2` parameter is ignored) or 32-bit. The function returns the instruction size in bytes (so the return value is either 2 or 4). Function `disasm_arm` always returns 4, because all ARM instructions are 32-bit.

After calling either function, the output (as a string) can be obtained with:

    const char *disarm_result(ARMSTATE *state);

The pointer that this function returns is to an internal buffer, which gets overwritten on the next call to `disasm_thumb` or `disasm_arm`. Therefore, you will probably need to make a copy of it.

## Decoding a buffer

A complete buffer (in memory) can be disassembled with one function call. 

    bool disasm_buffer(ARMSTATE *state,
                       const unsigned char *buffer, size_t buffersize, int mode,
                       DISASM_CALLBACK callback, void *user);

Function `disasm_buffer` disassembles an entire buffer with binary machine language. It calls a callback function for each instruction, with the text of that instruction. Flags set with `disasm_init` apply here too. Parameter `mode` is set to the initial mode (`ARMMODE_ARM` or `ARMMODE_THUMB`), but if symbols (with a known mode) were added, `disasm_buffer` switches the mode as soon as it crosses the address of the symbol.

    typedef bool (*DISASM_CALLBACK)(uint32_t address, const char *text, void *user);

On each call to the callback, parameter `address` has the memory address of the instruction, and `text` contains one line of disassembled output. Parameter `user` contains the value that was passed in to function `disasm_buffer`. The callback function must return `true` to proceed with disassembly (it will make `disasm_buffer` abort if it returns `false`).

In principle, using `disasm_buffer` is similar to calling the `disasm_thumb` or `disasm_arm` functions in a loop, but since `disasm_buffer` knows the buffer size, it can look up and down the memory block to improve parameter decoding.

# ARM mode versus Thumb mode

This disassembler library offers no help in determining whether to use ARM mode or Thumb mode. The mode cannot be determined from the instructions themselves. You have to already know the mode to disassemble the buffer correctly. The ARM micro-controller itself uses the low bit of the `PC` register to indicate Thumb mode, even though the Thumb instructions are not actually on an odd address.

The DWARF debugging information stores the real (even) function addresses (and does not store the mode). Hence, DWARF dymbolic information is of no help (for this matter). The MAP file created by the linker, likewise, reports the real (even) function addresses of all symbols.

However, the ELF file format also has a symbol table, and this table records the address of Thumb mode functions on an odd address. Therefore, for an educated guess of whether to use ARM or Thumb modes, you need the ELF file, and you need a library to read and parse the ELF symbol table. A small built-in parser for little-endian ARM ELF32 files and their `$a`/`$t`/`$d` mapping symbols is provided in `armelf.h`/`armelf.c`; see "Reading an ARM ELF section" below. (For other formats you still add the symbols yourself, as described next.)

Assuming that you have managed to read the ELF symbol table, the easiest way to pass this information to the disassembler library, is by adding each symbol with a call to `disasm_symbol`; see below for more on this function. Of note is that you need to correct the address of Thumb functions in the call to `disasm_symbol`; as stated above, the function is not really at an odd address. The ELF format uses the low bit in the address as a ARM/Thumb mode indicator.

A final note is that only function `disasm_buffer` uses the symbol table to select (and switch) the mode, based on the address. The `disasm_thumb` or `disasm_arm` functions ignore the symbol table.

# Symbolic information and string literals

If you have symbolic information for the binary code, you can add this with function `disasm_symbol` prior to starting to disassemble instructions.

    bool disasm_symbol(ARMSTATE *state, const char *name, uint32_t address, int mode);

Each call to this function adds a single symbol (either a function or a global variable). Only static addresses are supported, so local variables (with stack relative addresses) cannot be added. The purpose of adding these symbols is to show a function name on each `BL` or `BLX` instruction, or the name of a global variable on an `LDR` instruction. The option `DISASM_COMMENT` must also be set on `disarm_init` for this feature.

Parameter `mode` of function `disasm_symbol` is one of:

    ARMMODE_UNKNOWN     unknown mode for the symbol
    ARMMODE_ARM         this symbol refers to code in ARM mode (function)
    ARMMODE_THUMB       this symbol refers to code in Thumb mode (function)
    ARMMODE_DATA        this symbol refers to a data object

You can also mark only the mode for an address, by adding a symbol without a name. In this case, set the `name` parameter to `NULL` in the call to `disasm_symbol`.

The functions `disasm_thumb` and `disasm_arm` do not use the ARM/Thumb mode flags. However, function `disasm_buffer` uses it to switch between ARM mode and Thumb mode. See also the preceding section.

Function `disasm_buffer` also tries to look up literal strings, that the code refers to. For that end, `disasm_buffer` may need to look beyond the block that it has been passed (for disassembly). Many compilers store literal strings in the `.rodata` section in an ELF file. So, what you can do is to pass that section to `disasm_literals` before calling `disasm_buffer`.

    bool disasm_literals(ARMSTATE *state, const uint8_t *block, size_t blocksize, uint32_t address);

The function makes a copy of the block, so you may deallocate it after the call to `disasm_literals`. Multiple blocks may be added (these should not overlap, but no check is made whether they do). To clear the blocks with read-only data, call `disasm_cleanup` (which frees all memory, you need to call `disasm_init` again if you want to continue).

# Mapping symbols and mixed ARM/Thumb/data sections

A single executable section of an ARM ELF file can alternate between ARM code, Thumb code and inline data (literal pools, veneers). The ELF mapping symbols `$a`, `$t` and `$d` mark where each region starts; dotted variants such as `$a.1` or `$d.const` are accepted as well. Each marker owns the **half-open** range from its own address up to the next marker.

Mapping markers are stored independently of the ordinary named symbols, so a mapping never replaces a function name at the same address.

    bool disasm_mapping(ARMSTATE *state, uint32_t address, int mode);
    void disasm_clear_mappings(ARMSTATE *state);

Parameter `mode` is one of `ARMMODE_ARM`, `ARMMODE_THUMB` or `ARMMODE_DATA`. Markers are kept sorted on address. Adding the same address with the same mode merges with the existing marker (and returns `true`); adding the same address with a different mode is rejected and returns `false` without changing the list.

`disasm_buffer` consults the markers while decoding:

* Lookup only uses the marker at the current address and the markers **before** it. A later marker never changes the decoding mode ahead of its range.
* Without an active marker, the `mode` passed to `disasm_buffer` and the ordinary symbol based mode switching behave exactly as before.
* Inside an `ARMMODE_DATA` range no instruction is decoded; every byte is emitted, in address order, as `.byte 0xNN` with a two digit lower-case hex value.
* A 4-byte ARM instruction or a 2/4-byte Thumb instruction is never allowed to consume bytes from the next mapping range. Bytes at the end of a range that do not form a complete instruction are emitted as individual `.byte 0xNN` lines as well.
* Explicit code mappings override the speculative literal-pool (`POOL_LITERAL`) detection, so mapped code is always decoded as instructions. Data ranges never derive branches, symbols or literal pools from their bytes.
* Pending IT (if-then) predicate state is dropped when leaving Thumb code or when a byte fragment is emitted; returning to Thumb starts without stale predicates.

The callback address, `state.size` and the address/binary prefixes always match the bytes consumed by that line (1 for a `.byte` line, 2 or 4 for an instruction). A callback returning `false` still aborts `disasm_buffer`.

# Reading an ARM ELF section

`armelf.h`/`armelf.c` add a small, dependency free ELF32 loader:

    bool disasm_elf_section(ARMSTATE *state,
                            const uint8_t *image, size_t size,
                            uint32_t section_index,
                            DISASM_CALLBACK callback, void *user);

It supports little-endian `EM_ARM` ELF32 files of type `ET_REL` and `ET_EXEC`. The selected section must be an executable `SHT_PROGBITS` section (`SHF_EXECINSTR`). Every `SHT_SYMTAB` is located and its `sh_link` is used to find the associated string table. Only symbols with `st_shndx == section_index` are imported, so equally valued symbols from other sections cannot leak in.

Local `STT_NOTYPE` `$a`/`$t`/`$d` symbols (and dotted variants) become mapping markers. Named `STT_FUNC` symbols become ordinary comment symbols after clearing the Thumb low address bit. For `ET_REL`, `st_value` is a section offset; for `ET_EXEC` it is an absolute address; both are converted to the `sh_addr` coordinate of the selected section.

Every call rebuilds the ordinary symbols, mappings and codepool for the selected section while preserving the display option flags (`DISASM_ADDRESS`, `DISASM_INSTR`, `DISASM_COMMENT`). The same `ARMSTATE` can therefore be reused for another section, including a different section linked at the same address. The image is only read. Unsupported or malformed files/sections return `false`.

The ELF entry ends by calling the ordinary `disasm_buffer`; the installed mappings also keep working for a later direct `disasm_buffer` call.

## C example

```c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "armdisasm.h"
#include "armelf.h"

static bool print_line(uint32_t address, const char *text, void *user)
{
    (void)user;
    printf("%08x  %s\n", address, text);
    return true;            /* return false to stop disassembling */
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s firmware.elf [section-index]\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    rewind(fp);
    uint8_t *image = malloc((size_t)length);
    if (fread(image, 1, (size_t)length, fp) != (size_t)length) {
        fprintf(stderr, "short read\n");
        return 1;
    }
    fclose(fp);

    ARMSTATE state;
    disasm_init(&state, DISASM_ADDRESS | DISASM_COMMENT);

    int section = (argc > 2) ? atoi(argv[2]) : -1;
    if (section < 0) {
        /* Pick the first executable SHT_PROGBITS section. */
        uint32_t shoff = le32(image + 32);
        uint16_t entsize = le16(image + 46);
        uint16_t shnum = le16(image + 48);
        for (uint16_t i = 1; i < shnum && section < 0; i++) {
            const uint8_t *sh = image + shoff + (size_t)i * entsize;
            if (le32(sh + 4) == 1 /*SHT_PROGBITS*/ &&
                (le32(sh + 8) & 0x4 /*SHF_EXECINSTR*/))
                section = i;
        }
    }

    bool ok = false;
    if (section >= 0)
        ok = disasm_elf_section(&state, image, (size_t)length,
                                (uint32_t)section, print_line, NULL);
    if (!ok)
        fprintf(stderr, "unsupported ELF or bad section %d\n", section);

    disasm_cleanup(&state);
    free(image);
    return ok ? 0 : 2;
}
```

The same mapping API can be used without ELF data, for example when the ranges come from another loader:

```c
disasm_init(&state, DISASM_ADDRESS);
disasm_address(&state, 0x1000);
disasm_mapping(&state, 0x1000, ARMMODE_THUMB); /* [0x1000,0x1008) */
disasm_mapping(&state, 0x1008, ARMMODE_ARM);   /* [0x1008,0x100c) */
disasm_mapping(&state, 0x100c, ARMMODE_DATA); /* to buffer end   */
disasm_buffer(&state, buffer, sizeof buffer, ARMMODE_THUMB, cb, NULL);
disasm_cleanup(&state);
```

# Code pool

While disassembling, the ARM-disasm library inspects `LDR` instructions to find the start of literal pools, and it inspects the branch instructions to find the end of literal pools. Literal pools may appear in the middle of a function (with the code branching around it). It builds a map of known locations for instructions or literals, which it refers to as the "code pool".

When you throw completely unrelated bits of binary code at `disasm_thumb`, `disasm_arm` or `disasm_buffer`, you will need to clear the code pool in between those calls. Otherwise, there is a risk that an instruction isn't decoded, because the address was flagged as "literal pool" during the disassembly of an earlier block of code.

    void disasm_clear_codepool(ARMSTATE *state);

For example, the unit test for this library, which tests a bunch of instructions one by one, clears the code pool at the start of every test.

Alternatively, you can compact the code pool (or a range of it):

    void disasm_compact_codepool(ARMSTATE *state, uint32_t address, uint32_t size);

This function removes redundant entries in the codepool. Calling it is optional, but it optimizes processing the pool and reduces memory requirements. It should, however, only be called for address regions for which the codepool can be considered stable. For example, you can compact the address range of a full function after that function has been disassembled completely.

# Why build my own

I am aware of [pebble-disthumb](https://github.com/radare/pebble-disthumb) by pancake/radare.org, and of [DARM](https://github.com/jbremer/darm) by Jurriaan Bremer. The first, pebble-disthumb, is quite limited; but DARM also didn't get a couple of instructions right. DARM is not maintained anymore, and since I am not well versed in Python, the option of forking it and maintaining it myself was not attractive.

I am also aware of [Capstone](http://www.capstone-engine.org/). However, my goal was to "enrich" the disassembly with symbols and information extracted ELF and DWARF. I did not immediately see any "hooks" in Capstone to plug the symbolic information in. Plus, I felt that Capstone is overkill for a tiny debugger front-end that only supports ARM Cortex.

