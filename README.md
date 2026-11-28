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

Android shared libraries compress their relocation sections with the APS2 wire format.
JElf recognizes the two operating-system-specific section types and exposes them through the
existing relocation object model:

- `SHT_ANDROID_REL` (`0x60000001`) is parsed as an `ElfRelocationSection`.
- `SHT_ANDROID_RELA` (`0x60000002`) is parsed as an `ElfRelocationAddendSection`.

The compressed stream is expanded lazily and the entries are read from the same public
`relocations` array as an ordinary section. `sectionsOfType(ElfRelocationSection.class)` and
`sectionsOfType(ElfRelocationAddendSection.class)` therefore find packed sections too. The section
header's `sh_type` and the bytes returned by `getData()` keep their on-disk APS2 form; the image is
never rewritten into plain relocations. ELF32/64, both byte orders and negative addends are
supported, and the expansion order (including duplicate positions) is preserved.

Each relocation resolves its symbol through the owning relocation section's `sh_link`, and each
symbol's `getName()` follows that symbol table's own `sh_link` string table. A `.symtab` and a
`.dynsym` can therefore use different string tables; tables are never merged by name.

The following prints the expanded symbolic references of every packed RELA section:

```java
import net.fornwall.jelf.ElfFile;
import net.fornwall.jelf.ElfRelocationAddend;
import net.fornwall.jelf.ElfRelocationAddendSection;
import net.fornwall.jelf.ElfSectionHeader;
import net.fornwall.jelf.ElfSymbol;
import java.nio.file.Path;

public class PrintPackedRelocations {
    public static void main(String[] args) throws Exception {
        ElfFile elf = ElfFile.from(Path.of(args[0]));
        for (ElfRelocationAddendSection section :
                elf.sectionsOfType(ElfRelocationAddendSection.class)) {
            if (section.header.sh_type != ElfSectionHeader.SHT_ANDROID_RELA) {
                continue; // ordinary RELA sections use the same object model
            }
            System.out.println(section.header.getName());
            for (ElfRelocationAddend rel : section.relocations) {
                ElfSymbol symbol = rel.getSymbol();
                System.out.printf("  offset=0%x type=%d symbol=%s addend=%d%n",
                        rel.r_offset, rel.getType(),
                        symbol == null ? null : symbol.getName(), rel.r_addend);
            }
        }
    }
}
```

### Failure reporting

Malformed packed data is reported through `ElfException` before any partial relocation array is
returned, and one section failing never affects the other sections of the image. Every message
names the section and the byte position inside the compressed data. The reported conditions include:

- reads running past the end of the section and an unterminated or wider-than-file-width SLEB128;
- a bad or truncated `APS2` magic;
- unknown group flag bits, a non-positive `group_size`, `ADDEND` without `HAS_ADDEND`, or any addend
  flag in an `SHT_ANDROID_REL` stream;
- a group producing more records than the declared `relocation_count`;
- offset arithmetic leaving the unsigned file width or addend arithmetic leaving the signed width;
- trailing bytes after the declared number of relocations.

Packed sections are read-only views: JElf explains the symbolic references but never performs
dynamic loading, address patching or image execution.

## ELF Resources
- [Wikipedia entry on the ELF format](https://en.wikipedia.org/wiki/Executable_and_Linkable_Format)
- [elf(5) man page](http://man7.org/linux/man-pages/man5/elf.5.html)
