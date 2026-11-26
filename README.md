<p align="center">
    <img src="logo.png" alt="HeaderParser Logo" width="50%"/>
</p>

# Header Parser
Parses header information of a binary (executable) file.  
PE, ELF, DEX, MachO, ZIP (JAR, DocX) are parsed in depth.  
Java.class, ART, .NET, NE, MS-DOS are recognized.  

The focus was on PE and ELF. 
The other types are handled less carefully but may be extended in the future.
As well as PE and ELF still have to be extended.


POSIX compliant.  
Compiles and runs under
- Linux 
- Windows (x86/x64)  
- OsX may work too
- Android in Termux


## CONTENTS
- [VERSION](#version)
- [REQUIREMENTS](#requirements)
- [BUILD](#build)
    - [Linux (gcc) & cmake](#linux-(gcc)-&-cmake)
    - [Linux (gcc)](#linux-(gcc))
    - [Windows (MsBuild)](#windows-(msbuild))
    - [Runtime Errors (Windows)](#runtime-errors-(windows))
- [USAGE](#usage)
    - [Windows Context Menu](#windows-context-menu)
- [EXAMPLE](#example)
    - [Offsets](#offsets)
    - [Forcing](#forcing)
- [LIBRARY USAGE](#library-usage)
    - [Build](#build)
    - [Usage](#usage)
    - [Mach-O indirect symbol references](#mach-o-indirect-symbol-references)
    - [Python](#python)
- [COPYRIGHT, CREDITS & CONTACT](#copyright,-credits-&-contact)
    - [Author](#author)
    - [Co-Author, Icon Art](#co-author,-icon-art)


## VERSION
1.15.17  
Last changed: 22.07.2026  

## REQUIREMENTS
- Linux
   - Gcc
- Windows
   - msbuild


## BUILD
### Linux (gcc)
script
```bash
$ ./linuxBuild.sh [-t app] [-m Release|Debug] [-h]
```

manual
```bash
$ mkdir build
$ gcc -o build/headerParser -Wl,-z,relro,-z,now -D_FILE_OFFSET_BITS=64 -Ofast src/headerParser.c src/pe/PEHeader.c src/pe/PEHeaderOffsets.c
```

Use `clang` instead of `gcc` in Termux on Android.

### Windows (MsBuild)
```bash
$ winBuild.bat [/exe] [/m <Release|Debug>] [/b <32|64>] [/rtl] [/pdb] [/bt <path>] [/pts <PlatformToolset>] [/h]
```
This will run in a normal cmd.  

The correct path to your build tools may be passed  with the `/bt` parameter or changed in the script [winBuild.bat](winBuild.bat) itself.  

The PlatformToolset defaults to "v143", but may be changed with the `/pts` option.
"v142" is used for VS 2019, "v143" would be used in VS 2022.

In a developer cmd you can also type:
```bash
$devcmd> msbuild HeaderParser.vcxproj /p:Configuration=<Release|Debug> /p:Platform=<x64|x86> [/p:PlatformToolset=<v142|v143|WindowsApplicationForDrivers10.0>]
```

**Warnings**  
MSBuild issues some serious warnings:
- `headerParser\src\headerDataHandler.h(54): warning C6001: Using uninitialized memory`
- `headerParser\src\dex\DexHeaderParser.h(371): warning C6386: Buffer overrun`

But so far I could not figure out, how to fix them, or put another way, what's the problem.
If someone knows, feel free to drop me a line.

### Runtime Errors (Windows)
If a "VCRUNTIMExxx.dll not found Error" occurs on the target system, statically including runtime libs is a solution.  
This is done by using the `/p:RunTimeLib=Debug|Release` (msbuild) or `[/rtl]` (winBuild) flags.

 

## USAGE
```bash
$ ./headerParser a/file/name [options]
$ ./headerParser [options] a/file/name
```
Options:  
 * -h Print help.
 * -s:uint64_t Start offset in file. Default = 1.
 * -i:uint8_t Level of output info. 1 : minimal output (Default), 2 : extended output (basic header).
 * -f:string Force parsing a specific type, skipping magic value checks. Currently, only "pe" is supported.
 * -offs: show file offsets of the printed values (for -i 2 or XX only options).
 * PE only options:
   * -dosh: Print DOS header.
   * -coffh: Print COFF header.
   * -opth: Print Optional header.
   * -sech: Print Section headers.
   * -exp: Print the Image Export Table (IMAGE_DIRECTORY_ENTRY_EXPORT).
   * -imp: Print the Image Import Table (IMAGE_DIRECTORY_ENTRY_IMPORT) dll names and info.
   * -impx: Print the Image Import Table (IMAGE_DIRECTORY_ENTRY_IMPORT) dll names, info and imported functions.
   * -res: Print the Image Resource Table (IMAGE_DIRECTORY_ENTRY_RESOURCE).
   * -crt: Print the Image Certificate Table (IMAGE_DIRECTORY_ENTRY_CERTIFICATE).
   * -cod: Directory to save found certificates in. (Needs -crt.)
   * -rel: Print the Image Base Relocation Table (IMAGE_DIRECTORY_ENTRY_BASE_RELOC).
   * -dbg: Print the Debug Table (IMAGE_DIRECTORY_ENTRY_DEBUG).
   * -dbgx: Print the Debug Table (IMAGE_DIRECTORY_ENTRY_DEBUG) extended.
   * -tls: Print the Image TLS Table (IMAGE_DIRECTORY_ENTRY_TLS).
   * -lcfg: Print the Image Load Config Table (IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
   * -bimp: Print the Image Bound Import Table (IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT).
   * -dimp: Print the Image Delay Import Table (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) dll names and info.
   * -dimpx: Print the Image Delay Import Table (IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) dll names, info and imported functions.
 * ELF only options:
   * -fileh: Print file header.
   * -progh: Print program headers.
   * -sech: Print section headers.
   * -sym: Print symbol table (names only).
   * -symx: Print symbol table with all info.
   * -dym: Print dynamic symbol table (names only).
   * -dymx: Print dynamic symbol table with all info.
 * DEX only options:
   * -fileh: Print file header.
   * -class: Print class defs.
   * -field: Print field ids.
   * -map: Print map.
   * -method: Print method ids.
   * -proto: Print proto ids.
   * -string: Print string ids.
   * -type: Print type ids.

### Windows Context Menu
It may be convenient to add HeaderParser to the context menu to be able to right-click a file and header parse it.
In this scenario, you may use [addBinShellCtxtMenuEntry.bat](https://github.com/fkie-cad/windowsScripts/blob/master/addBinShellCtxtMenuEntry.bat)
```bash
$ addBinShellCtxtMenuEntry.bat /p "c:\HeaderParser.exe" /l "Open in HeaderParser"
```



## EXAMPLE
```bash
$ ./headerParser a/file/name [-i 1]

HeaderData:
coderegions:
 (1) .text: ( 0x0000000000000400 - 0x000000000000fc00 )
 (2) .init ...
 (3) ...
headertype: PE|ELF|... (32|64)
bitness: 64-bit|32-bit|x-bit
endian: little|big
CPU_arch: Intel|Arm|...
Machine: ...
```

There is a difference between the header bitness (displayed in brackets following the `headertype`) and the bitness of the executable (program code). 
The header bitness is 32 or 64 bit for ELF, MACH-O and PE. 
The bitness of the executable (program code) may be different though.

An extended output will be printed, by setting "-i 2", which will cover the basic headers.
```bash
$ ./headerParser a/file.exe -i 2

PE Image Dos Header:
...
Coff File Header:
...
Optional Header::
...
Section Header:
1 / x
...
2 / x
```

```bash
$ ./headerParser an/elf/file -i 2

ELF File header:
...
Program Header Table:
1 / x
...
2 / x
...
Section Header Table:
1 / y
...
2 / y
...
```

A more fine-grained and/or extended printout is available with the PE or ELF only options.

### Offsets
If you think, the header starts somewhere in the file, you may pass an offset to it using the "-s" option.

### Forcing
If you think it is a PE file but the MZ or PE00 magic values are broken, try the "-f pe" option.


## LIBRARY USAGE
HeaderParser may also be built as a shared or static library.  

### Build
*Linux*
```bash
$ ./linuxBuild.sh -t sh [-m Release|Debug] [-h]
or
$ ./linuxBuild.sh -t st [-m Release|Debug] [-h]
```
or plain:  
shared  
```bash
$ mkdir build
$ gcc -fPIC -Wl,-z,relro,-z,now -shared -Ofast -D_FILE_OFFSET_BITS=64 -Wall -o build/libheaderparser.so src/headerParserLib.c src/pe/PEHeader.c src/pe/PEHeaderOffsets.c
```

static  
```bash
$ mkdir build
$ gcc -fPIC -Wl,-z,relro,-z,now -Ofast -D_FILE_OFFSET_BITS=64 -c -Wall -o build/headerParserLib.o src/headerParserLib.c 
$ gcc -fPIC -Wl,-z,relro,-z,now -Ofast -D_FILE_OFFSET_BITS=64 -c -Wall -o build/PEHeader.o src/pe/PEHeader.c
$ gcc -fPIC -Wl,-z,relro,-z,now -Ofast -D_FILE_OFFSET_BITS=64 -c -Wall -o build/PEHeaderOffsets.o src/pe/PEHeaderOffsets.c
$ ar rcs build/headerParser.a build/*.o
```

*Windows*
```bash
$ winBuild.bat /dll [/m Release|Debug] [/b 32|64]
// or
$ winBuild.bat /lib [/m Release|Debug] [/b 32|64]
```

### Usage
Additionally to the included header files `src\exp.h` is needed in the same directory.
This may be removed soon.

```c
// link library when compiling
// include
#include "src/HeaderData.h"
#include "src/headerParserLib.h"
...
// use library
size_t offset = 0;
uint8_t force = FORCE_NONE; // or FORCE_PE
HeaderData* data = getBasicHeaderParserInfo("a/file.path", offset, force);
if ( data )
{
// do stuff handling data
// ...
}
// clean up
freeHeaderData(data);
```

For PE files there is an extended parser available. 
This one includes the basic data info.

layout
```
src/
    HeaderData.h
    PEHeaderData.h
    headerParserLibPE.h
    pe/PEHeader.h
```

```c
// include
#include "src/HeaderData.h"
#include "src/PEHeaderData.h"
#include "src/headerParserLibPE.h"
...
// use library
size_t offset = 0;
PEHeaderData* data = getPEHeaderData("a/file.path", offset);
if ( data )
{
// do stuff handling data
// ...
}
// clean up
freePEHeaderData(data);
```

### Mach-O indirect symbol references

For thin 32/64 bit Mach-O images of either endianness, `getBasicHeaderParserInfo()`
resolves the relation between indirect symbol slots and their symbol/library:

- `S_NON_LAZY_SYMBOL_POINTERS` (type `6`)
- `S_LAZY_SYMBOL_POINTERS` (type `7`) — slot width is the image bit width / 8
  (`4` bytes on 32 bit, `8` bytes on 64 bit)
- `S_SYMBOL_STUBS` (type `8`) — slot/stub width is `reserved2`

`reserved1` is the first indirect table index of the section. Output order is
physical section order (segment commands and their section records), then slot
order. Symbols with the same name are never merged. Only static on-disk
structure is analyzed: referenced dylibs are not opened and the image is never
loaded or executed.

`HeaderData` adds:

```c
MachOReferenceData* macho_references;   /* NULL when size is 0 */
size_t              macho_references_size;
uint8_t             macho_references_error; /* MACHO_REFERENCES_OK or ..._ERR_* */
```

Field units and meaning of `MachOReferenceData`:

| Field | Unit / base | Meaning |
| --- | --- | --- |
| `segment_index` | ordinal, **1 based** | physical segment command number |
| `section_index` | ordinal, **1 based** | physical section number over *all* sections |
| `section_index_in_segment` | ordinal, **1 based** | section number inside its segment |
| `segment_name`, `section_name` | owned C string | 16 byte Mach-O names, NUL terminated |
| `section_type` | enum low byte | `6`, `7` or `8` |
| `slot_index` | index, **0 based** | slot inside the section |
| `address` | virtual **bytes** | section `addr + slot * slot_width` |
| `image_offset` | file **bytes** | position relative to the Mach-O image start |
| `file_offset` | file **bytes** | position relative to the whole file (`start + image_offset`) |
| `indirect_index` | table index | section `reserved1 + slot_index` |
| `indirect_value` | raw `uint32_t` | endian corrected indirect table word |
| `status` | `MachOReferenceStatus` | resolved / unresolved / local / abs / local+abs |
| `nlist_index` | nlist index | raw ordinary indirect value |
| `symbol_name` | owned C string or `NULL` | copied from the `LC_SYMTAB` string table |
| `n_type`, `n_sect` | raw bytes | original nlist fields |
| `n_desc` | raw `uint16_t` | endian corrected |
| `n_value` | **bytes/address value** | raw nlist value, widened to 64 bit for 32 bit images |
| `library_ordinal` | ordinal or `-1` | decoded bits `n_desc[15:8]` |
| `library_kind` | `MachOLibraryOrdinalKind` | normal / self / dynamic lookup / executable / invalid |
| `library_path` | owned C string or `NULL` | load order dylib path for positive ordinals |

Special indirect values keep their own state and are never interpreted as nlist
indexes: `INDIRECT_SYMBOL_LOCAL` (`0x80000000`) → `MACHO_REFERENCE_LOCAL`,
`INDIRECT_SYMBOL_ABS` (`0x40000000`) → `MACHO_REFERENCE_ABS`, both bits
(`0xC0000000`) → `MACHO_REFERENCE_LOCAL_ABS`.

Library ordinals count only loading class dylib commands in file order:
`LC_LOAD_DYLIB`, `LC_LOAD_WEAK_DYLIB`, `LC_REEXPORT_DYLIB` and
`LC_LOAD_UPWARD_DYLIB`. `LC_ID_DYLIB` is not counted. Ordinal `0` is *self*,
`0xfe` is *dynamic lookup* and `0xff` is the *executable*
(`MACHO_LIBRARY_ORDINAL_SELF/DYNAMIC_LOOKUP/EXECUTABLE`). Positive ordinals map
to the corresponding dylib path.

If an individual nlist index, string offset or library ordinal cannot be
linked, the slot and its raw numbers are still returned with
`status == MACHO_REFERENCE_UNRESOLVED`; no name is fabricated. A zero stub
width, a section size that is not a whole number of slots, or indirect/table
ranges that do not cover the slots set `macho_references_error` to a
`MachOReferencesError` code; the array is then empty rather than a partial
table, while the basic Mach-O header fields remain readable. Images without
these section types return `MACHO_REFERENCES_OK` with size `0`.

When `start` passed to `getBasicHeaderParserInfo()` is non zero,
`symoff`, `stroff`, `indirectsymoff` and section offsets are relative to the
image start; `file_offset` additionally includes `start`.

The array and all strings are owned by the `HeaderData` object: they are heap
copies, so closing/deleting the file and parsing another image never changes
already returned results. Release everything with `freeHeaderData()`.

A complete compilable example lives at
[`examples/macho_references_example.c`](examples/macho_references_example.c):

```c
#include "src/headerParserLib.h"

HeaderData* hd = getBasicHeaderParserInfo("image.bin", 0, FORCE_NONE);
if (hd && hd->macho_references_error == MACHO_REFERENCES_OK) {
    for (size_t i = 0; i < hd->macho_references_size; ++i) {
        MachOReferenceData* r = &hd->macho_references[i];
        printf("%s slot %u: %s (%s), ordinal %d\n",
               r->section_name, r->slot_index,
               r->symbol_name ? r->symbol_name : "<special>",
               r->library_path ? r->library_path : "-",
               r->library_ordinal);
    }
}
freeHeaderData(hd);
```

The extended command line output (`headerParser -i 2 image`) prints the same
relations as `MachOIndirectReferences (n):`.

### Python
Using the library is the preferred usage in python.  
On the python side, use [header_parser.py](src/header_parser.py).
```python
from src import header_parser

# initialization
header_parser.init("src/of/libheaderparser.so")
# default usage
data = header_parser.get_basic_info('a/file.src')
# passing a start offset
data = header_parser.get_basic_info('a/file.src', 10)
# passing a start offset and forcing PE parsing
data = header_parser.get_basic_info('a/file.src', 10, header_parser.FORCE_PE)
# convert cpu id and header type id into strings
cpu = header_parser.lib_header_parser.getHeaderDataHeaderType(data['cpu'])
header_type = header_parser.lib_header_parser.getHeaderDataArchitecture(data['headertype'])
```

## COPYRIGHT, CREDITS & CONTACT
Published under [GNU GENERAL PUBLIC LICENSE](LICENSE).

### Author
- Henning Braun ([henning.braun@fkie.fraunhofer.de](mailto:henning.braun@fkie.fraunhofer.de)) 

### Co-Author, Icon Art
common_codeio.h, Icon.ico
- Viviane Zwanger ([viviane.zwanger@fkie.fraunhofer.de](mailto:viviane.zwanger@fkie.fraunhofer.de))
