package net.fornwall.jelf;

import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.List;

/**
 * Regression contract for Android {@code APS2} packed relocation sections.
 *
 * <p>The cases build small inert ELF images in memory and check the results through the public
 * {@link ElfFile}/{@link ElfRelocationSection}/{@link ElfRelocationAddendSection} API. Nothing is
 * mapped, patched or executed.
 */
public final class PackedContract {

    private PackedContract() {}

    public static TaskCheck.Case[] cases() {
        return new TaskCheck.Case[] {
            // Grouping semantics.
            new TaskCheck.Case("groups", "mixedGroupsCrossBoundaries", PackedContract::mixedGroups),
            new TaskCheck.Case("groups", "relKeepsOrderAndDuplicates", PackedContract::relDuplicates),
            new TaskCheck.Case("groups", "negativeAddendsAndGroupedAddendOnce", PackedContract::negativeAddends),
            new TaskCheck.Case("groups", "emptyStreamStillHasInitialOffset", PackedContract::emptyStream),
            new TaskCheck.Case("groups", "unsigned32BitOffsetPatterns", PackedContract::unsignedOffsets),
            // Symbol and string table ownership.
            new TaskCheck.Case("ownership", "perSectionSymbolAndStringTables", PackedContract::ownership),
            new TaskCheck.Case("ownership", "interleavedReadsAreConsistent", PackedContract::interleavedReads),
            // Compatibility with ordinary relocation and all four file layouts.
            new TaskCheck.Case("compatibility", "fourFileLayouts", PackedContract::fourFileLayouts),
            new TaskCheck.Case("compatibility", "ordinaryAndPackedRecordsAgree", PackedContract::ordinaryAndPacked),
            new TaskCheck.Case("compatibility", "malformedStreamsAreReported", PackedContract::malformedStreams),
            new TaskCheck.Case("compatibility", "failureDoesNotPoisonOtherSections", PackedContract::failureIsolation),
        };
    }

    // ---------------------------------------------------------------------------------------
    // Helpers

    private static long rinfo(boolean wide, int symbolIndex, long type) {
        return wide ? ((long) symbolIndex << 32) | (type & 0xffffffffL) : ((long) symbolIndex << 8) | (type & 0xff);
    }

    /**
     * Builds a 64-bit little-endian image with a symbol table and one packed relocation section at
     * section index 5.
     */
    private static ElfFile packedFile(boolean wide, ByteOrder order, int sectionType, byte[] stream) {
        Fixture.Image image = new Fixture.Image(wide, order);
        image.add(".text", ElfSectionHeader.SHT_PROGBITS, new byte[8], 0, 0, 0);
        int strtab = image.add(
                ".strtab",
                ElfSectionHeader.SHT_STRTAB,
                Fixture.strings("sym0", "sym1", "sym2", "sym3", "sym4", "sym5", "alpha"),
                0,
                0,
                0);
        int symtab = image.add(
                ".symtab",
                ElfSectionHeader.SHT_SYMTAB,
                image.symbols(
                        image.entry(1, 0x10),
                        image.entry(2, 0x20),
                        image.entry(3, 0x30),
                        image.entry(4, 0x40),
                        image.entry(5, 0x50),
                        image.entry(6, 0x60),
                        image.entry(7, 0x70)),
                strtab,
                1,
                image.entrySize());
        String name = sectionType == ElfSectionHeader.SHT_ANDROID_RELA ? ".android.rela" : ".android.rel";
        image.add(name, sectionType, stream, symtab, 0, 1);
        return ElfFile.from(image.bytes());
    }

    private static ElfRelocationAddendSection packedRela(ElfFile file) {
        return (ElfRelocationAddendSection) file.getSection(5);
    }

    private static ElfRelocationSection packedRel(ElfFile file) {
        return (ElfRelocationSection) file.getSection(5);
    }

    private static void checkRelArrays(
            TaskCheck.Check c,
            String what,
            long[] expectedOffsets,
            long[] expectedInfos,
            long[] actualOffsets,
            long[] actualInfos) {
        c.check(
                Arrays.equals(expectedOffsets, actualOffsets),
                what + " offsets expected=" + Arrays.toString(expectedOffsets) + " actual="
                        + Arrays.toString(actualOffsets));
        c.check(
                Arrays.equals(expectedInfos, actualInfos),
                what + " infos expected=" + Arrays.toString(expectedInfos) + " actual="
                        + Arrays.toString(actualInfos));
    }

    // ---------------------------------------------------------------------------------------
    // Group semantics

    private static void mixedGroups(TaskCheck.Check c) {
        boolean wide = true;
        long i1 = rinfo(wide, 1, 1), i2 = rinfo(wide, 2, 2), i3 = rinfo(wide, 3, 3);
        long i4 = rinfo(wide, 4, 4), i5 = rinfo(wide, 5, 5);
        byte[] stream = Fixture.packed(
                7, 100, // relocation_count, initial_offset
                2, 3, 10, i1, // group 1: shared offset delta + shared info
                1, 0, 0, i2, // group 2: everything per record
                2, 15, 5, i3, 100, // group 3: shared offset delta, info and addend delta
                1, 8, -20, i4, -30, // group 4: per record addend delta
                1, 1, i5, 7); // group 5: shared info, addend resets to zero
        ElfRelocationAddendSection section = packedRela(packedFile(wide, ByteOrder.LITTLE_ENDIAN,
                ElfSectionHeader.SHT_ANDROID_RELA, stream));

        c.check(section.relocations.length == 7, "expected 7 relocations");
        checkRelArrays(
                c,
                "mixed",
                new long[] {110, 120, 120, 125, 130, 110, 117},
                new long[] {i1, i1, i2, i3, i3, i4, i5},
                offsets(section),
                infos(section));
        long[] addends = addends(section);
        c.check(
                Arrays.equals(new long[] {0, 0, 0, 100, 100, 70, 0}, addends),
                "mixed addends=" + Arrays.toString(addends));

        // Running offset persists across group boundaries and duplicates are kept in order.
        c.check(section.relocations[1].r_offset == 120 && section.relocations[2].r_offset == 120,
                "duplicate offsets must be preserved");
        // Grouped addend delta is applied once, not per record.
        c.check(section.relocations[3].r_addend == 100 && section.relocations[4].r_addend == 100,
                "grouped addend delta applied once");
    }

    private static void relDuplicates(TaskCheck.Check c) {
        boolean wide = true;
        long i8 = rinfo(wide, 1, 8), i9 = rinfo(wide, 1, 9), i10 = rinfo(wide, 1, 10);
        byte[] stream = Fixture.packed(
                4, 0,
                2, 3, 8, i8, // shared offset delta and shared info
                2, 0, 0, i9, 0, i10); // per record offset and info
        ElfRelocationSection section = packedRel(packedFile(wide, ByteOrder.LITTLE_ENDIAN,
                ElfSectionHeader.SHT_ANDROID_REL, stream));

        checkRelArrays(
                c,
                "rel",
                new long[] {8, 16, 16, 16},
                new long[] {i8, i8, i9, i10},
                offsets(section),
                infos(section));
        c.check(section.relocations[2].r_offset == section.relocations[3].r_offset, "duplicate kept");
        c.check(section.relocations[2].getType() == 9 && section.relocations[3].getType() == 10,
                "REL types decoded");
        c.check(section.header.sh_type == ElfSectionHeader.SHT_ANDROID_REL, "sh_type preserved");
    }

    private static void negativeAddends(TaskCheck.Check c) {
        boolean wide = true;
        long i2 = rinfo(wide, 1, 2);
        byte[] stream = Fixture.packed(
                3, 0,
                2, 15, 0, i2, -10, // grouped addend delta -10, two records share it
                1, 10, 5, i2, -5); // grouped offset delta, per-record addend delta -5
        ElfRelocationAddendSection section = packedRela(packedFile(wide, ByteOrder.LITTLE_ENDIAN,
                ElfSectionHeader.SHT_ANDROID_RELA, stream));

        checkRelArrays(c, "negative", new long[] {0, 0, 5}, new long[] {i2, i2, i2},
                offsets(section), infos(section));
        long[] addends = addends(section);
        c.check(
                Arrays.equals(new long[] {-10, -10, -15}, addends),
                "negative addends=" + Arrays.toString(addends));
    }

    private static void emptyStream(TaskCheck.Check c) {
        boolean wide = true;
        byte[] stream = Fixture.packed(0, 4660); // count zero still carries initial_offset
        ElfFile file = packedFile(wide, ByteOrder.LITTLE_ENDIAN, ElfSectionHeader.SHT_ANDROID_RELA, stream);
        ElfRelocationAddendSection section = packedRela(file);
        c.check(section.relocations.length == 0, "empty stream has no relocations");
        c.check(Arrays.equals(stream, section.getData()), "raw bytes unchanged");
        c.check(section.header.sh_type == ElfSectionHeader.SHT_ANDROID_RELA, "sh_type unchanged");

        ElfRelocationSection relSection = packedRel(packedFile(wide, ByteOrder.LITTLE_ENDIAN,
                ElfSectionHeader.SHT_ANDROID_REL, Fixture.packed(0, 0)));
        c.check(relSection.relocations.length == 0, "empty REL stream has no relocations");
    }

    private static void unsignedOffsets(TaskCheck.Check c) {
        boolean wide = false;
        long i2 = rinfo(wide, 1, 2), i3 = rinfo(wide, 1, 3);
        byte[] stream = Fixture.packed(
                2, -1, // initial offset is the unsigned 0xffffffff pattern
                1, 3, 0, i2, // offset stays 0xffffffff
                1, 3, -1, i3); // offset walks back to 0xfffffffe
        ElfRelocationAddendSection section = packedRela(packedFile(wide, ByteOrder.LITTLE_ENDIAN,
                ElfSectionHeader.SHT_ANDROID_RELA, stream));

        c.check(section.relocations[0].r_offset == 0xFFFFFFFFL,
                "unsigned 32-bit offset 0x" + Long.toHexString(section.relocations[0].r_offset));
        c.check(section.relocations[1].r_offset == 0xFFFFFFFEL,
                "unsigned 32-bit offset 0x" + Long.toHexString(section.relocations[1].r_offset));
        c.check(section.relocations[0].r_info == i2 && section.relocations[1].r_info == i3, "info patterns");
    }

    // ---------------------------------------------------------------------------------------
    // Ownership

    private record Owner(ElfFile file, byte[] relStream, byte[] relaStream) {}

    private static Owner ownerImage() {
        boolean wide = true;
        ByteOrder order = ByteOrder.LITTLE_ENDIAN;
        Fixture.Image image = new Fixture.Image(wide, order);
        image.add(".text", ElfSectionHeader.SHT_PROGBITS, new byte[32], 0, 0, 0);
        int strtab = image.add(
                ".strtab", ElfSectionHeader.SHT_STRTAB, Fixture.strings("symtabName"), 0, 0, 0);
        int symtab = image.add(
                ".symtab",
                ElfSectionHeader.SHT_SYMTAB,
                image.symbols(image.entry(1, 0x10)),
                strtab,
                1,
                image.entrySize());
        int dynstr = image.add(
                ".dynstr", ElfSectionHeader.SHT_STRTAB, Fixture.strings("dynsymName"), 0, 0, 0);
        int dynsym = image.add(
                ".dynsym",
                ElfSectionHeader.SHT_DYNSYM,
                image.symbols(image.entry(1, 0x20)),
                dynstr,
                1,
                image.entrySize());
        int thirdstr = image.add(
                ".thirdstr", ElfSectionHeader.SHT_STRTAB, Fixture.strings("thirdName"), 0, 0, 0);
        int thirdtab = image.add(
                ".thirdtab",
                ElfSectionHeader.SHT_SYMTAB,
                image.symbols(image.entry(1, 0x30)),
                thirdstr,
                1,
                image.entrySize());
        image.add(
                ".rela.own",
                ElfSectionHeader.SHT_RELA,
                image.rela(8, image.info(1, 2), -3),
                dynsym,
                1,
                wide ? 24 : 12);
        image.add(
                ".rel.own",
                ElfSectionHeader.SHT_REL,
                image.rel(16, image.info(1, 4)),
                symtab,
                1,
                wide ? 16 : 8);
        byte[] relStream = Fixture.packed(1, 0, 1, 3, 24, image.info(1, 5));
        image.add(".android.rel", ElfSectionHeader.SHT_ANDROID_REL, relStream, thirdtab, 1, 1);
        byte[] relaStream = Fixture.packed(1, 0, 1, 15, 32, image.info(1, 6), -7);
        image.add(".android.rela", ElfSectionHeader.SHT_ANDROID_RELA, relaStream, dynsym, 1, 1);
        return new Owner(ElfFile.from(image.bytes()), relStream, relaStream);
    }

    private static void ownership(TaskCheck.Check c) {
        Owner owner = ownerImage();
        ElfFile file = owner.file;

        ElfSymbolTableSection symtab = (ElfSymbolTableSection) file.getSection(4);
        ElfSymbolTableSection dynsym = (ElfSymbolTableSection) file.getSection(6);
        ElfSymbolTableSection thirdtab = (ElfSymbolTableSection) file.getSection(8);
        ElfRelocationAddendSection ordinaryRela = (ElfRelocationAddendSection) file.getSection(9);
        ElfRelocationSection ordinaryRel = (ElfRelocationSection) file.getSection(10);
        ElfRelocationSection androidRel = (ElfRelocationSection) file.getSection(11);
        ElfRelocationAddendSection androidRela = (ElfRelocationAddendSection) file.getSection(12);

        c.check(symtab.symbols.length == 2, "symtab has null + one symbol");
        c.check(dynsym.symbols.length == 2, "dynsym has null + one symbol");
        c.check(thirdtab.symbols.length == 2, "third table has null + one symbol");
        c.check(symtab.symbols[1].st_name == 1 && dynsym.symbols[1].st_name == 1
                && thirdtab.symbols[1].st_name == 1, "all tables use the same name index");
        c.check("symtabName".equals(symtab.symbols[1].getName()), "regular table name");
        c.check("dynsymName".equals(dynsym.symbols[1].getName()), "dynamic table name");
        c.check("thirdName".equals(thirdtab.symbols[1].getName()), "third table name");
        c.check(symtab.symbols[1].st_value == 0x10 && dynsym.symbols[1].st_value == 0x20
                && thirdtab.symbols[1].st_value == 0x30, "symbol values distinguish tables");

        // Ordinary records follow the owning relocation section's sh_link.
        ElfSymbol fromOrdinaryRela = ordinaryRela.relocations[0].getSymbol();
        c.check(fromOrdinaryRela == dynsym.symbols[1], "ordinary RELA uses its linked dynsym object");
        c.check("dynsymName".equals(fromOrdinaryRela.getName()), "ordinary RELA symbol name");
        ElfSymbol fromOrdinaryRel = ordinaryRel.relocations[0].getSymbol();
        c.check(fromOrdinaryRel == symtab.symbols[1], "ordinary REL uses its linked symtab object");
        c.check("symtabName".equals(fromOrdinaryRel.getName()), "ordinary REL symbol name");

        // Packed records follow their own section's sh_link, not the first or any other table.
        ElfSymbol fromPackedRel = androidRel.relocations[0].getSymbol();
        c.check(fromPackedRel == thirdtab.symbols[1], "packed REL uses the third table object");
        c.check("thirdName".equals(fromPackedRel.getName()), "packed REL symbol name");
        ElfSymbol fromPackedRela = androidRela.relocations[0].getSymbol();
        c.check(fromPackedRela == dynsym.symbols[1], "packed RELA uses dynsym object");
        c.check("dynsymName".equals(fromPackedRela.getName()), "packed RELA symbol name");

        // Old public entry points are unchanged.
        c.check(file.getSymbolTableSection() == symtab, "getSymbolTableSection returns first symtab");
        c.check(file.getDynamicSymbolTableSection() == dynsym, "getDynamicSymbolTableSection returns dynsym");

        c.check(androidRel.relocations[0].r_offset == 24, "packed REL offset");
        c.check(androidRela.relocations[0].r_offset == 32 && androidRela.relocations[0].r_addend == -7,
                "packed RELA offset/addend");
    }

    private static void interleavedReads(TaskCheck.Check c) {
        Owner owner = ownerImage();
        ElfFile file = owner.file;
        ElfRelocationAddendSection ordinaryRela = (ElfRelocationAddendSection) file.getSection(9);
        ElfRelocationSection ordinaryRel = (ElfRelocationSection) file.getSection(10);
        ElfRelocationSection androidRel = (ElfRelocationSection) file.getSection(11);
        ElfRelocationAddendSection androidRela = (ElfRelocationAddendSection) file.getSection(12);
        ElfSymbolTableSection symtab = (ElfSymbolTableSection) file.getSection(4);
        ElfSymbolTableSection dynsym = (ElfSymbolTableSection) file.getSection(6);

        for (int round = 0; round < 3; round++) {
            c.check(Arrays.equals(owner.relStream, androidRel.getData()), "android REL raw bytes round " + round);
            c.check(Arrays.equals(owner.relaStream, androidRela.getData()),
                    "android RELA raw bytes round " + round);
            c.check(androidRel.header.sh_type == ElfSectionHeader.SHT_ANDROID_REL, "REL sh_type round " + round);
            c.check(androidRela.header.sh_type == ElfSectionHeader.SHT_ANDROID_RELA,
                    "RELA sh_type round " + round);

            c.check(file.getSection(11) == androidRel && file.getSection(12) == androidRela,
                    "memoized sections stay identical round " + round);
            c.check(androidRela.relocations[0].getSymbol() == dynsym.symbols[1],
                    "RELA symbol identity round " + round);
            c.check(androidRel.relocations[0].getSymbol() == ((ElfSymbolTableSection) file.getSection(8)).symbols[1],
                    "REL symbol identity round " + round);
            c.check(ordinaryRela.relocations[0].getSymbol() == dynsym.symbols[1],
                    "ordinary RELA symbol identity round " + round);
            c.check(ordinaryRel.relocations[0].getSymbol() == symtab.symbols[1],
                    "ordinary REL symbol identity round " + round);
            c.check("dynsymName".equals(androidRela.relocations[0].getSymbol().getName()),
                    "RELA symbol name round " + round);
            c.check("thirdName".equals(androidRel.relocations[0].getSymbol().getName()),
                    "REL symbol name round " + round);
            c.check(ordinaryRela.relocations[0].r_offset == 8, "ordinary RELA offset round " + round);
            c.check(androidRel.relocations[0].r_offset == 24, "packed REL offset round " + round);
        }
    }

    // ---------------------------------------------------------------------------------------
    // Compatibility

    private record Layout(byte[] bytes, byte[] packedRela, byte[] packedRel) {}

    private static Layout layout(boolean wide, ByteOrder order) {
        Fixture.Image image = new Fixture.Image(wide, order);
        image.add(".text", ElfSectionHeader.SHT_PROGBITS, new byte[32], 0, 0, 0);
        int strtab = image.add(".strtab", ElfSectionHeader.SHT_STRTAB, Fixture.strings("alpha"), 0, 0, 0);
        int symtab = image.add(
                ".symtab",
                ElfSectionHeader.SHT_SYMTAB,
                image.symbols(image.entry(1, 16)),
                strtab,
                1,
                image.entrySize());
        image.add(
                ".rela.text",
                ElfSectionHeader.SHT_RELA,
                Fixture.concat(image.rela(8, image.info(1, 2), -4), image.rela(16, image.info(1, 3), 5)),
                symtab,
                2,
                wide ? 24 : 12);
        image.add(
                ".rel.text",
                ElfSectionHeader.SHT_REL,
                Fixture.concat(image.rel(24, image.info(1, 4)), image.rel(24, image.info(1, 5))),
                symtab,
                2,
                wide ? 16 : 8);
        byte[] packedRela = Fixture.packed(
                3, 0,
                2, 15, 4, image.info(1, 6), 7,
                1, 11, 8, image.info(1, 7), -3);
        image.add(".android.rela", ElfSectionHeader.SHT_ANDROID_RELA, packedRela, symtab, 3, 1);
        byte[] packedRel = Fixture.packed(
                4, 0,
                2, 3, 8, image.info(1, 8),
                2, 0, 0, image.info(1, 9), 0, image.info(1, 10));
        image.add(".android.rel", ElfSectionHeader.SHT_ANDROID_REL, packedRel, symtab, 4, 1);
        return new Layout(image.bytes(), packedRela, packedRel);
    }

    private static void fourFileLayouts(TaskCheck.Check c) {
        boolean[] widths = {false, true};
        ByteOrder[] orders = {ByteOrder.LITTLE_ENDIAN, ByteOrder.BIG_ENDIAN};
        for (boolean wide : widths) {
            for (ByteOrder order : orders) {
                String tag = (wide ? "ELF64" : "ELF32") + "/" + order;
                Layout layout = layout(wide, order);
                ElfFile file = ElfFile.from(layout.bytes);
                long i6 = rinfo(wide, 1, 6), i7 = rinfo(wide, 1, 7);
                long i8 = rinfo(wide, 1, 8), i9 = rinfo(wide, 1, 9), i10 = rinfo(wide, 1, 10);

                List<ElfRelocationAddendSection> relaSections =
                        file.sectionsOfType(ElfRelocationAddendSection.class);
                c.check(relaSections.size() == 2, tag + " two RELA-typed sections");
                c.check(".rela.text".equals(relaSections.get(0).header.getName()), tag + " ordinary RELA name");
                c.check(".android.rela".equals(relaSections.get(1).header.getName()), tag + " android RELA name");
                c.check(relaSections.get(0).header.sh_type == ElfSectionHeader.SHT_RELA, tag + " SHT_RELA kept");
                c.check(relaSections.get(1).header.sh_type == ElfSectionHeader.SHT_ANDROID_RELA,
                        tag + " SHT_ANDROID_RELA kept");
                ElfRelocationAddendSection androidRela = relaSections.get(1);
                c.check(Arrays.equals(layout.packedRela, androidRela.getData()), tag + " RELA raw bytes unchanged");
                checkRelArrays(c, tag + " android RELA", new long[] {4, 8, 16}, new long[] {i6, i6, i7},
                        offsets(androidRela), infos(androidRela));
                c.check(Arrays.equals(new long[] {7, 7, 4}, addends(androidRela)), tag + " android RELA addends");

                List<ElfRelocationSection> relSections = file.sectionsOfType(ElfRelocationSection.class);
                c.check(relSections.size() == 2, tag + " two REL-typed sections");
                c.check(".rel.text".equals(relSections.get(0).header.getName()), tag + " ordinary REL name");
                c.check(".android.rel".equals(relSections.get(1).header.getName()), tag + " android REL name");
                c.check(relSections.get(1).header.sh_type == ElfSectionHeader.SHT_ANDROID_REL,
                        tag + " SHT_ANDROID_REL kept");
                ElfRelocationSection androidRel = relSections.get(1);
                c.check(Arrays.equals(layout.packedRel, androidRel.getData()), tag + " REL raw bytes unchanged");
                checkRelArrays(c, tag + " android REL", new long[] {8, 16, 16, 16}, new long[] {i8, i8, i9, i10},
                        offsets(androidRel), infos(androidRel));

                List<ElfSection> androidRelaByType =
                        file.sectionsOfType(ElfSectionHeader.SHT_ANDROID_RELA);
                c.check(androidRelaByType.size() == 1 && androidRelaByType.get(0) == androidRela,
                        tag + " integer lookup finds android RELA");
                List<ElfSection> androidRelByType = file.sectionsOfType(ElfSectionHeader.SHT_ANDROID_REL);
                c.check(androidRelByType.size() == 1 && androidRelByType.get(0) == androidRel,
                        tag + " integer lookup finds android REL");

                ElfSymbolTableSection symtab = file.getSymbolTableSection();
                c.check(androidRela.relocations[0].getSymbol() == symtab.symbols[1], tag + " symbol identity");
                c.check("alpha".equals(androidRela.relocations[0].getSymbol().getName()), tag + " symbol name");
                c.check(androidRela.relocations[0].getType() == 6 && androidRel.relocations[0].getType() == 8,
                        tag + " relocation types");

                ElfRelocationAddendSection ordinaryRela = relaSections.get(0);
                c.check(ordinaryRela.relocations.length == 2 && ordinaryRela.relocations[0].r_offset == 8
                        && ordinaryRela.relocations[0].r_addend == -4, tag + " ordinary RELA unaffected");
            }
        }
    }

    private static void ordinaryAndPacked(TaskCheck.Check c) {
        for (boolean wide : new boolean[] {false, true}) {
            for (ByteOrder order : new ByteOrder[] {ByteOrder.LITTLE_ENDIAN, ByteOrder.BIG_ENDIAN}) {
                String tag = (wide ? "ELF64" : "ELF32") + "/" + order;
                Fixture.Image image = new Fixture.Image(wide, order);
                image.add(".text", ElfSectionHeader.SHT_PROGBITS, new byte[32], 0, 0, 0);
                int strtab = image.add(".strtab", ElfSectionHeader.SHT_STRTAB,
                        Fixture.strings("alpha"), 0, 0, 0);
                int symtab = image.add(".symtab", ElfSectionHeader.SHT_SYMTAB,
                        image.symbols(image.entry(1, 16)), strtab, 1, image.entrySize());
                long i12 = image.info(1, 2), i13 = image.info(1, 3);
                image.add(".rela.text", ElfSectionHeader.SHT_RELA,
                        Fixture.concat(
                                image.rela(8, i12, -4),
                                image.rela(16, i13, 5),
                                image.rela(16, i13, 5)),
                        symtab, 3, wide ? 24 : 12);
                byte[] stream = Fixture.packed(
                        3, 8,
                        1, 15, 0, i12, -4,
                        2, 8, 8, i13, 9, 0, i13, 0);
                image.add(".android.rela", ElfSectionHeader.SHT_ANDROID_RELA, stream, symtab, 3, 1);
                ElfFile file = ElfFile.from(image.bytes());

                ElfRelocationAddendSection ordinary = (ElfRelocationAddendSection) file.getSection(5);
                ElfRelocationAddendSection packed = (ElfRelocationAddendSection) file.getSection(6);
                c.check(ordinary.relocations.length == 3 && packed.relocations.length == 3,
                        tag + " record counts");
                for (int i = 0; i < 3; i++) {
                    c.check(ordinary.relocations[i].r_offset == packed.relocations[i].r_offset,
                            tag + " offset " + i);
                    c.check(ordinary.relocations[i].r_info == packed.relocations[i].r_info, tag + " info " + i);
                    c.check(ordinary.relocations[i].r_addend == packed.relocations[i].r_addend,
                            tag + " addend " + i);
                    c.check(ordinary.relocations[i].getSymbol() == packed.relocations[i].getSymbol(),
                            tag + " symbol object " + i);
                    c.check("alpha".equals(packed.relocations[i].getSymbol().getName()), tag + " symbol name " + i);
                }
                c.check(packed.relocations[1].r_offset == packed.relocations[2].r_offset,
                        tag + " duplicate position");
            }
        }
    }

    private static void malformedStreams(TaskCheck.Check c) {
        byte[] magic = {65, 80, 83, 50};

        expectFailure(c, "bad magic", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(new byte[] {65, 80, 83, 51}, Fixture.sleb(0), Fixture.sleb(0)), ".android.rela");
        expectFailure(c, "short header", true, ElfSectionHeader.SHT_ANDROID_RELA,
                new byte[] {65, 80, 83}, ".android.rela");
        expectFailure(c, "empty stream", true, ElfSectionHeader.SHT_ANDROID_RELA,
                new byte[0], ".android.rela");
        expectFailure(c, "unterminated relocation count", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(magic, new byte[] {(byte) 0x80}), ".android.rela");
        expectFailure(c, "over-wide SLEB128 count", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(magic, new byte[] {
                    (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80,
                    (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0
                }), ".android.rela");
        expectFailure(c, "over-wide SLEB128 count in ELF32", false, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(magic, new byte[] {
                    (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0
                }), ".android.rela");
        expectFailure(c, "invalid sign extension", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(magic, new byte[] {
                    (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80,
                    (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 1
                }), ".android.rela");
        expectFailure(c, "invalid sign extension in ELF32", false, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(magic, new byte[] {
                    (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0x0F
                }), ".android.rela");
        expectFailure(c, "negative count", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(-1, 0), ".android.rela");
        expectFailure(c, "zero group size", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, 0, 3, 0, rinfo(true, 1, 2)), ".android.rela");
        expectFailure(c, "negative group size", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, -1, 3, 0, rinfo(true, 1, 2)), ".android.rela");
        expectFailure(c, "unknown flag bits", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, 1, 16), ".android.rela");
        expectFailure(c, "ADDEND without HAS_ADDEND", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, 1, 4), ".android.rela");
        expectFailure(c, "addend flags in REL", true, ElfSectionHeader.SHT_ANDROID_REL,
                Fixture.packed(1, 0, 1, 8), ".android.rel");
        expectFailure(c, "group exceeds declared count", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, 2, 0, 0, rinfo(true, 1, 2)), ".android.rela");
        expectFailure(c, "32-bit offset overflow", false, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, -1, 1, 3, 2, rinfo(false, 1, 2)), ".android.rela");
        expectFailure(c, "64-bit offset underflow", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, 1, 2, -1), ".android.rela");
        expectFailure(c, "64-bit offset overflow", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, -1, 1, 2, 1), ".android.rela");
        expectFailure(c, "32-bit addend overflow", false, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(2, 0, 1, 15, 0, rinfo(false, 1, 2), Integer.MAX_VALUE, 1, 8, 0, rinfo(false, 1, 2), 1),
                ".android.rela");
        expectFailure(c, "64-bit addend overflow", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(2, 0, 1, 15, 0, rinfo(true, 1, 2), Long.MAX_VALUE, 1, 8, 0, rinfo(true, 1, 2), 1),
                ".android.rela");
        expectFailure(c, "trailing bytes", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(0, 0, 0), ".android.rela");
        expectFailure(c, "truncated group header", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(1, 0, 1, 3), ".android.rela");
        expectFailure(c, "truncated before initial offset", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.concat(magic, Fixture.sleb(1)), ".android.rela");
        expectFailure(c, "truncated second record", true, ElfSectionHeader.SHT_ANDROID_RELA,
                Fixture.packed(2, 0, 2, 0, 5, rinfo(true, 1, 2)), ".android.rela");
    }

    private static void expectFailure(
            TaskCheck.Check c,
            String what,
            boolean wide,
            int sectionType,
            byte[] stream,
            String sectionName) {
        ElfFile file = packedFile(wide, ByteOrder.LITTLE_ENDIAN, sectionType, stream);
        try {
            file.getSection(5);
            c.check(false, what + ": expected ElfException");
        } catch (ElfException e) {
            String message = e.getMessage();
            c.check(message != null && message.contains(sectionName),
                    what + ": message should name section, was: " + message);
            c.check(message != null && message.matches(".*byte \\d+.*"),
                    what + ": message should carry a byte position, was: " + message);
        } catch (RuntimeException e) {
            c.check(false, what + ": expected ElfException but got " + e.getClass().getName() + ": " + e.getMessage());
        }
    }

    private static void failureIsolation(TaskCheck.Check c) {
        Fixture.Image image = new Fixture.Image(true, ByteOrder.LITTLE_ENDIAN);
        image.add(".text", ElfSectionHeader.SHT_PROGBITS, new byte[16], 0, 0, 0);
        int strtab = image.add(".strtab", ElfSectionHeader.SHT_STRTAB,
                Fixture.strings("good"), 0, 0, 0);
        int symtab = image.add(".symtab", ElfSectionHeader.SHT_SYMTAB,
                image.symbols(image.entry(1, 0x77)), strtab, 1, 24);
        byte[] bad = Fixture.concat(new byte[] {65, 80, 83, 51}, Fixture.sleb(0), Fixture.sleb(0));
        image.add(".android.rela", ElfSectionHeader.SHT_ANDROID_RELA, bad, symtab, 1, 1);
        byte[] goodRelStream = Fixture.packed(1, 0, 1, 3, 4, image.info(1, 2));
        image.add(".android.rel", ElfSectionHeader.SHT_ANDROID_REL, goodRelStream, symtab, 1, 1);
        image.add(".rela.text", ElfSectionHeader.SHT_RELA,
                image.rela(8, image.info(1, 2), -4), symtab, 3, 24);
        ElfFile file = ElfFile.from(image.bytes());

        ElfRelocationSection goodRel = (ElfRelocationSection) file.getSection(6);
        ElfRelocationAddendSection goodRela = (ElfRelocationAddendSection) file.getSection(7);
        ElfSymbolTableSection table = (ElfSymbolTableSection) file.getSection(4);
        long goodOffset = goodRel.relocations[0].r_offset;

        for (int attempt = 0; attempt < 2; attempt++) {
            try {
                file.getSection(5);
                c.check(false, "attempt " + attempt + ": expected ElfException");
            } catch (ElfException e) {
                c.check(e.getMessage().contains(".android.rela"), "attempt " + attempt + ": section name");
            }
            c.check(file.getSection(6) == goodRel, "attempt " + attempt + ": good REL memoized");
            c.check(goodRel.relocations[0].r_offset == goodOffset, "attempt " + attempt + ": good REL values");
            c.check(goodRel.relocations[0].getSymbol() == table.symbols[1],
                    "attempt " + attempt + ": good REL symbol");
            c.check("good".equals(goodRel.relocations[0].getSymbol().getName()),
                    "attempt " + attempt + ": good REL name");
            c.check(goodRela.relocations[0].r_addend == -4, "attempt " + attempt + ": good RELA");
            c.check(Arrays.equals(goodRelStream, goodRel.getData()), "attempt " + attempt + ": good raw bytes");
        }
    }

    // ---------------------------------------------------------------------------------------
    // Accessors shared by the checks

    private static long[] offsets(ElfRelocationSection section) {
        long[] result = new long[section.relocations.length];
        for (int i = 0; i < result.length; i++) result[i] = section.relocations[i].r_offset;
        return result;
    }

    private static long[] infos(ElfRelocationSection section) {
        long[] result = new long[section.relocations.length];
        for (int i = 0; i < result.length; i++) result[i] = section.relocations[i].r_info;
        return result;
    }

    private static long[] offsets(ElfRelocationAddendSection section) {
        long[] result = new long[section.relocations.length];
        for (int i = 0; i < result.length; i++) result[i] = section.relocations[i].r_offset;
        return result;
    }

    private static long[] infos(ElfRelocationAddendSection section) {
        long[] result = new long[section.relocations.length];
        for (int i = 0; i < result.length; i++) result[i] = section.relocations[i].r_info;
        return result;
    }

    private static long[] addends(ElfRelocationAddendSection section) {
        long[] result = new long[section.relocations.length];
        for (int i = 0; i < result.length; i++) result[i] = section.relocations[i].r_addend;
        return result;
    }
}
