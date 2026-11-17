# JElf
Java library for parsing [Executable and Linkable Format (ELF)](https://en.wikipedia.org/wiki/Executable_and_Linkable_Format) files.

[![Build Status](https://github.com/fornwall/jelf/workflows/Java%20CI/badge.svg)](https://github.com/fornwall/jelf/actions?query=workflow%3A%22Java+CI%22)
[![MIT licensed](http://img.shields.io/:license-MIT-blue.svg)](LICENSE.txt)
[![Package on Maven Central](https://img.shields.io/maven-central/v/net.fornwall/jelf)](https://search.maven.org/artifact/net.fornwall/jelf/)
[![javadoc](https://www.javadoc.io/badge/net.fornwall/jelf.svg)](https://www.javadoc.io/doc/net.fornwall/jelf)

## Adding JElf to your build

JElf's Maven group ID is `net.fornwall` and its artifact ID is `jelf`.

To add a dependency on JElf using Maven, use the following:

```xml
<dependency>
    <groupId>net.fornwall</groupId>
    <artifactId>jelf</artifactId>
    <version>0.12.0</version>
</dependency>
```

To add a dependency using Gradle:

```gradle
dependencies {
    implementation 'net.fornwall:jelf:0.12.0'
}
```

## Using JElf
See the [ElfFile](https://www.javadoc.io/doc/net.fornwall/jelf/latest/net/fornwall/jelf/ElfFile.html) class for how to parse and query an ELF file.

## Android packed relocations (APS2)

Android shared libraries may store relocations in a compressed `APS2` stream instead of a plain
array of `Elf32_Rel`/`Elf64_Rela` records. JElf decodes these sections into the existing relocation
model:

| `sh_type` | Constant | Section class | Entries |
| --- | --- | --- | --- |
| `0x60000001` | `ElfSectionHeader.SHT_ANDROID_REL` | `ElfRelocationSection` | `ElfRelocation[]` (no addends) |
| `0x60000002` | `ElfSectionHeader.SHT_ANDROID_RELA` | `ElfRelocationAddendSection` | `ElfRelocationAddend[]` |

The expanded entries are read from the usual `relocations` array of those sections, and
`sectionsOfType(ElfRelocationSection.class)`, `sectionsOfType(ElfRelocationAddendSection.class)` and
the integer `sectionsOfType(0x60000001/0x60000002)` overloads all return them. Nothing is written
back to the file: `header.sh_type`, `header.sh_size` and `getData()` keep exposing the original
on-disk `APS2` bytes, and plain `SHT_REL`/`SHT_RELA` sections behave exactly as before.

The decoder follows the grouping contract in
[`task_support/FORMAT.md`](task_support/FORMAT.md):

- A `APS2` magic is followed by SLEB128 `relocation_count`, `initial_offset`, then groups.
- The running offset accumulates across all groups and records; duplicate positions are kept and
  records appear in wire order.
- A group without the `HAS_ADDEND` flag resets the running addend to zero. With `HAS_ADDEND` and
  `ADDEND`, the shared addend delta is applied once before the group's records; with only
  `HAS_ADDEND`, each record carries its own addend delta.
- REL sections never guess addends from target bytes; any addend flag in a REL section is an error.
- Both ELF classes, both byte orders and negative addends are supported. 32/64-bit offset and
  addend arithmetic is validated against the file word width.

Malformed input is reported as an `ElfException` naming the section and the byte position within
the compressed data. No partially decoded array is ever handed out. Covered failures include:
out-of-range reads, unterminated or over-wide SLEB128 integers, invalid flag combinations
(unknown bits, `ADDEND` without `HAS_ADDEND`, any addend flag in REL), groups exceeding the declared
relocation count, offset/addend arithmetic leaving the file word range, and trailing bytes. Each
section is decoded independently, so a failing section never affects other sections.

Symbol resolution follows each relocation section's `sh_link`: `getSymbol()` looks the index up in
the symbol table the relocation section links to, and `ElfSymbol.getName()` reads from the string
table that symbol table's own `sh_link` references. A regular symbol table and a dynamic symbol
table can therefore use different string tables, and interleaved reads remain consistent.

Example:

```java
import java.nio.file.Path;
import java.util.List;
import net.fornwall.jelf.*;

// Read-only inspection: the image is never mapped, patched or executed.
ElfFile elf = ElfFile.from(Path.of("libfoo.so"));

for (ElfRelocationAddendSection section : elf.sectionsOfType(ElfRelocationAddendSection.class)) {
    System.out.println(section.header.getName() + " (sh_type=0x"
            + Long.toHexString(section.header.sh_type) + ") has " + section.relocations.length
            + " entries; raw bytes=" + section.getData().length);
    for (ElfRelocationAddend rel : section.relocations) {
        ElfSymbol symbol = rel.getSymbol();
        String name = symbol.getName();
        System.out.printf("  offset=0x%x type=%d symbol=%s addend=%d%n",
                rel.r_offset, rel.getType(), name == null ? "<no symbol>" : name, rel.r_addend);
    }
}

for (ElfRelocationSection section : elf.sectionsOfType(ElfRelocationSection.class)) {
    System.out.println(section.header.getName() + " has " + section.relocations.length + " entries");
    for (ElfRelocation rel : section.relocations) {
        System.out.printf("  offset=0x%x type=%d%n", rel.r_offset, rel.getType());
    }
}
```

The above iterates ordinary and Android-packed REL/RELA sections alike. Viewing such a library is
read-only: JElf never maps, patches or executes the image.

## ELF Resources
- [Wikipedia entry on the ELF format](https://en.wikipedia.org/wiki/Executable_and_Linkable_Format)
- [elf(5) man page](http://man7.org/linux/man-pages/man5/elf.5.html)
