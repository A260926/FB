package net.fornwall.jelf;

import java.io.ByteArrayOutputStream;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.List;

/**
 * Regression contract for Android packed (APS2) relocation sections.
 *
 * <p>Cases are registered in three groups:
 * <ul>
 *     <li>{@code groups} - APS2 wire decoding: mixed flag groups, empty streams, high-bit patterns and failures.</li>
 *     <li>{@code ownership} - symbol/string table resolution through {@code sh_link}, without name based merging.</li>
 *     <li>{@code compatibility} - ordinary REL/RELA behavior, section discovery and raw byte preservation.</li>
 * </ul>
 */
public final class PackedContract {

    private static final ByteOrder[] ORDERS = {ByteOrder.LITTLE_ENDIAN, ByteOrder.BIG_ENDIAN};
    private static final boolean[] WIDTHS = {false, true};

    public static TaskCheck.Case[] cases() {
        return new TaskCheck.Case[] {
            // ---- groups ----
            new TaskCheck.Case("groups", "mixedGroupsAcrossBoundaries", PackedContract::mixedGroups),
            new TaskCheck.Case("groups", "emptyStreamKeepsInitialOffset", PackedContract::emptyStream),
            new TaskCheck.Case("groups", "highBitPatternsAllWidths", PackedContract::highBitPatterns),
            new TaskCheck.Case("groups", "addendCarriesAndResetsAcrossGroups", PackedContract::addendCarry),
            new TaskCheck.Case("groups", "maxWidthSlebEncodings", PackedContract::maxWidthSleb),
            new TaskCheck.Case("groups", "malformedStreamsReportedWithPosition", PackedContract::malformedStreams),
            new TaskCheck.Case("groups", "failuresStayInsideOneSection", PackedContract::failureIsolation),
            // ---- ownership ----
            new TaskCheck.Case("ownership", "shLinkSelectsSymbolTable", PackedContract::shLinkSymbolTables),
            new TaskCheck.Case("ownership", "sameIndexDifferentNamesPerTable", PackedContract::sameIndexDifferentNames),
            new TaskCheck.Case("ownership", "stringTablesResolvedByLinkNotName", PackedContract::stringTableByLink),
            new TaskCheck.Case("ownership", "repeatedAccessReturnsSameObjects", PackedContract::sameObjectIdentity),
            new TaskCheck.Case("ownership", "invalidShLinkIsElfException", PackedContract::invalidShLink),
            // ---- compatibility ----
            new TaskCheck.Case("compatibility", "ordinaryRelAndRelaUnchanged", PackedContract::ordinaryUnchanged),
            new TaskCheck.Case("compatibility", "sectionsOfTypeDiscoversPacked", PackedContract::sectionsOfType),
            new TaskCheck.Case("compatibility", "fourLayoutsInterleavedReads", PackedContract::interleavedReads),
            new TaskCheck.Case("compatibility", "rawBytesAndHeaderTypePreserved", PackedContract::rawViewPreserved),
            new TaskCheck.Case("compatibility", "relNeverReadsAddends", PackedContract::relRejectsAddends),
        };
    }

    // ------------------------------------------------------------------
    // groups
    // ------------------------------------------------------------------

    private static void mixedGroups(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            for (ByteOrder order : ORDERS) {
                Fixture.Mixed m = Fixture.mixed(wide, order);
                byte[] original = m.bytes.clone();
                ElfFile f = ElfFile.from(m.bytes);

                ElfRelocationAddendSection packedRela =
                        (ElfRelocationAddendSection) f.getSection(m.packedRela);
                c.check(packedRela.relocations.length == m.relaExpectations.length, "rela count");
                for (int i = 0; i < m.relaExpectations.length; i++) {
                    ElfRelocationAddend r = packedRela.relocations[i];
                    long[] e = m.relaExpectations[i];
                    c.check(r.r_offset == e[0], "rela offset " + i);
                    c.check(r.r_info == e[1], "rela info " + i);
                    c.check(r.r_addend == e[2], "rela addend " + i);
                    c.check(r.getSymbolIndex() <= 2, "rela sym idx " + i);
                }
                // Duplicate positions and expansion order are preserved.
                c.check(packedRela.relocations[0].r_offset == packedRela.relocations[1].r_offset, "dup rela offset");
                c.check(packedRela.relocations[4].r_offset == packedRela.relocations[5].r_offset, "dup rela offset 2");
                c.check(packedRela.relocations[2].r_addend == -5, "per-record addend carries over then adds -1");
                c.check(packedRela.relocations[3].r_addend == 0, "per-record addend accumulates +5");
                c.check(packedRela.relocations[4].r_addend == 0, "addend reset without HAS_ADDEND");

                ElfRelocationSection packedRel = (ElfRelocationSection) f.getSection(m.packedRel);
                c.check(packedRel.relocations.length == m.relExpectations.length, "rel count");
                for (int i = 0; i < m.relExpectations.length; i++) {
                    ElfRelocation r = packedRel.relocations[i];
                    long[] e = m.relExpectations[i];
                    c.check(r.r_offset == e[0], "rel offset " + i);
                    c.check(r.r_info == e[1], "rel info " + i);
                }
                c.check(packedRel.relocations[0].r_offset == packedRel.relocations[1].r_offset, "dup rel offset");
                c.check(packedRel.relocations[2].r_offset == packedRel.relocations[3].r_offset, "dup rel offset 2");

                c.check(Arrays.equals(m.bytes, original), "image bytes untouched after expansion");
            }
        }
    }

    private static void emptyStream(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            for (ByteOrder order : ORDERS) {
                for (int type : new int[] {Fixture.ANDROID_REL, Fixture.ANDROID_RELA}) {
                    byte[] image = Fixture.emptyImage(wide, order, type);
                    ElfFile f = ElfFile.from(image);
                    ElfSection section = f.firstSectionByType(type);
                    c.check(section != null, "empty packed section present");
                    if (type == Fixture.ANDROID_REL) {
                        c.check(((ElfRelocationSection) section).relocations.length == 0, "empty REL records");
                    } else {
                        c.check(((ElfRelocationAddendSection) section).relocations.length == 0, "empty RELA records");
                    }
                    // An empty stream still carries relocation_count and initial_offset.
                    c.check(section.getData().length > 4, "initial offset retained on disk");
                }
            }
        }
    }

    private static void highBitPatterns(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            for (ByteOrder order : ORDERS) {
                long[][] expected = Fixture.highBitExpectations(wide);
                for (int type : new int[] {Fixture.ANDROID_REL, Fixture.ANDROID_RELA}) {
                    ElfFile f = ElfFile.from(Fixture.highBitImage(wide, order, type));
                    if (type == Fixture.ANDROID_REL) {
                        ElfRelocation[] rs = f.firstSectionByType(ElfRelocationSection.class).relocations;
                        c.check(rs.length == 2, "high bit REL count");
                        for (int i = 0; i < 2; i++) {
                            c.check(rs[i].r_offset == expected[i][0], "high bit REL offset");
                            c.check(rs[i].r_info == expected[i][1], "high bit REL info");
                            int expectedIndex = (int) (wide ? expected[i][1] >>> 32 : expected[i][1] >>> 8);
                            c.check(rs[i].getSymbolIndex() == expectedIndex, "high bit REL symbol index");
                        }
                    } else {
                        ElfRelocationAddend[] rs =
                                f.firstSectionByType(ElfRelocationAddendSection.class).relocations;
                        c.check(rs.length == 2, "high bit RELA count");
                        for (int i = 0; i < 2; i++) {
                            c.check(rs[i].r_offset == expected[i][0], "high bit RELA offset");
                            c.check(rs[i].r_info == expected[i][1], "high bit RELA info");
                            c.check(rs[i].r_addend == 0, "high bit RELA addend");
                            int expectedIndex = (int) (wide ? expected[i][1] >>> 32 : expected[i][1] >>> 8);
                            c.check(rs[i].getSymbolIndex() == expectedIndex, "high bit RELA symbol index");
                        }
                    }
                }
            }
        }
    }

    private static void addendCarry(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            for (ByteOrder order : ORDERS) {
                long info = Fixture.info(wide, 1, 2);
                // A: grouped addend -4. B: per-record deltas carrying A's -4 forward.
                // C: no addend group resets to zero. D: per-record delta accumulates from zero again.
                byte[] stream = Fixture.packed(wide, 6, 0x1000,
                        2, 0xC, -4, 0x8, info, 0x8, info,
                        2, 0x8, 0x8, info, -1, 0x8, info, 5,
                        1, 0x0, 0x8, info,
                        1, 0x8, 0x8, info, 7);
                ElfFile f = ElfFile.from(
                        Fixture.singlePackedImage(wide, order, Fixture.ANDROID_RELA, stream));
                ElfRelocationAddendSection section =
                        (ElfRelocationAddendSection) f.getSection(Fixture.singlePackedIndex());
                long[] expectedAddends = {-4, -4, -5, 0, 0, 7};
                long expectedOffset = 0x1008;
                for (int i = 0; i < expectedAddends.length; i++) {
                    ElfRelocationAddend r = section.relocations[i];
                    c.check(r.r_addend == expectedAddends[i], "carried addend " + i);
                    c.check(r.r_offset == expectedOffset, "carried offset " + i);
                    c.check(r.r_info == info, "carried info " + i);
                    expectedOffset += 0x8;
                }
            }
        }
    }

    private static void maxWidthSleb(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            int maxLebBytes = wide ? 10 : 5;
            long signedMax = wide ? Long.MAX_VALUE : Integer.MAX_VALUE;
            long signedMin = wide ? Long.MIN_VALUE : Integer.MIN_VALUE;
            long info = Fixture.info(wide, 1, 2);
            for (long initial : new long[] {signedMax, signedMin}) {
                ByteArrayOutputStream payload = new ByteArrayOutputStream();
                payload.writeBytes(Fixture.leb(wide, 2)); // relocation_count
                payload.writeBytes(Fixture.leb(wide, initial)); // max-width initial offset
                payload.writeBytes(Fixture.leb(wide, 2)); // group_size
                payload.writeBytes(Fixture.leb(wide, 0x3)); // grouped OFFSET_DELTA | INFO
                payload.writeBytes(Fixture.leb(wide, 0x10)); // offset_delta
                payload.writeBytes(Fixture.leb(wide, info)); // r_info
                byte[] stream = Fixture.aps2(payload.toByteArray());

                // The initial offset really uses every allowed SLEB byte.
                c.check(Fixture.leb(wide, initial).length == maxLebBytes, "max-width SLEB length");
                byte[] image = Fixture.singlePackedImage(wide, ByteOrder.LITTLE_ENDIAN,
                        Fixture.ANDROID_REL, stream);
                ElfRelocationSection section = (ElfRelocationSection) ElfFile.from(image)
                        .getSection(Fixture.singlePackedIndex());
                long expectedOffset = (initial + 0x10) & (wide ? -1L : 0xffffffffL);
                for (ElfRelocation r : section.relocations) {
                    c.check(r.r_offset == expectedOffset, "max-width initial offset expansion");
                    c.check(r.r_info == info, "max-width stream r_info");
                }
            }
        }
    }

    private static void malformedStreams(TaskCheck.Check c) {
        // 32-bit and 64-bit, opposite endianness, take different SLEB width limits.
        Object[][] widthCases = {
            {false, ByteOrder.LITTLE_ENDIAN},
            {true, ByteOrder.BIG_ENDIAN},
        };
        for (Object[] wc : widthCases) {
            boolean wide = (Boolean) wc[0];
            ByteOrder order = (ByteOrder) wc[1];
            long info = Fixture.info(wide, 1, 2);

            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    new byte[] {'A', 'P', 'S'}), "truncated magic");
            byte[] badMagic = Fixture.aps2(new byte[] {1, 0});
            badMagic[0] = 'X';
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA, badMagic), "bad magic");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.aps2(new byte[] {(byte) 0x80})), "unterminated SLEB");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, -1, 0)), "negative relocation_count");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 1, 0, 0, 0)), "zero group_size");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 1, 0, 1, 0x10)), "unknown flag bit");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 1, 0, 1, 0x4)), "ADDEND without HAS_ADDEND");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_REL,
                    Fixture.packed(wide, 1, 0, 1, 0x8)), "addend flag in REL");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 1, 0, 2, 0)), "group larger than relocation_count");

            ByteArrayOutputStream trailing = new ByteArrayOutputStream();
            trailing.writeBytes(Fixture.packed(wide, 0, 0));
            trailing.write(0);
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    trailing.toByteArray()), "trailing bytes");

            // Offset arithmetic must stay in the unsigned file width. Both widths start at the all-ones
            // pattern; the delta wraps past 2^width-1 (by 2 for 32-bit, by 1 for 64-bit).
            long overflowDelta = wide ? 1 : 2;
            long startOffset = -1L;
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 1, startOffset, 1, 0, overflowDelta, info)), "offset out of file width");

            // A delta that merely crosses the signed boundary but stays unsigned is accepted.
            byte[] crossing = Fixture.packed(true, 1, Long.MAX_VALUE, 1, 0, 1, info);
            ElfFile crossingFile = ElfFile.from(
                    Fixture.singlePackedImage(true, ByteOrder.BIG_ENDIAN, Fixture.ANDROID_RELA, crossing));
            ElfRelocationAddendSection crossingSection = (ElfRelocationAddendSection) crossingFile
                    .getSection(Fixture.singlePackedIndex());
            c.check(crossingSection.relocations[0].r_offset == Long.MIN_VALUE,
                    "offset crossing 2^63 stays an unsigned in-range bit pattern");

            // Addend arithmetic must stay in the signed file width: per-record deltas that leave
            // the range, in both the positive and negative direction.
            long infoForAddend = info;
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 2, 0, 2, 0x8, 0x4, infoForAddend, wide ? Long.MAX_VALUE : Integer.MAX_VALUE,
                            0x4, infoForAddend, 1)), "positive addend out of file width");
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 2, 0, 2, 0x8, 0x4, infoForAddend, wide ? Long.MIN_VALUE : Integer.MIN_VALUE,
                            0x4, infoForAddend, -1)), "negative addend out of file width");
            // A second grouped addend delta pushing the running addend past the signed width is rejected.
            long maxAddend = wide ? Long.MAX_VALUE : Integer.MAX_VALUE;
            expectFailure(c, Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.packed(wide, 2, 0, 1, 0xC, maxAddend, 0x4, infoForAddend,
                            1, 0xC, 1, 0x4, infoForAddend)),
                    "grouped addend out of file width");
        }

        // SLEB exceeding the 32-bit file width: five continuation bytes followed by a terminator
        // carrying bits beyond bit 31.
        byte[] tooWide = Fixture.aps2(new byte[] {
            1, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0x10});
        expectFailure(c, Fixture.withBadSection(false, ByteOrder.LITTLE_ENDIAN,
                Fixture.ANDROID_RELA, tooWide), "SLEB wider than 32 bits");

        // Exactly five bytes but with invalid sign extension on the final byte: its high bits do
        // not match the sign bit of the four carried value bits.
        byte[] badSignExtension = Fixture.aps2(new byte[] {
            1, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0x30});
        expectFailure(c, Fixture.withBadSection(false, ByteOrder.LITTLE_ENDIAN,
                Fixture.ANDROID_RELA, badSignExtension), "invalid 32-bit SLEB sign extension");

        // The same five-byte shape with matching sign extension (payload 0x7f -> all ones above
        // bit 3) decodes as the signed minimum width value. relocation_count is zero so no groups follow.
        byte[] goodSignExtension = Fixture.aps2(new byte[] {
            0, (byte) 0x80, (byte) 0x80, (byte) 0x80, (byte) 0x80, 0x7f});
        ElfFile signFile = ElfFile.from(Fixture.singlePackedImage(false, ByteOrder.LITTLE_ENDIAN,
                Fixture.ANDROID_RELA, goodSignExtension));
        ElfRelocationAddendSection signSection =
                (ElfRelocationAddendSection) signFile.getSection(Fixture.singlePackedIndex());
        c.check(signSection.relocations.length == 0, "valid sign-extended max-width SLEB accepted");
    }

    private static void failureIsolation(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            ByteOrder order = wide ? ByteOrder.BIG_ENDIAN : ByteOrder.LITTLE_ENDIAN;
            byte[] image = Fixture.withBadSection(wide, order, Fixture.ANDROID_RELA,
                    Fixture.aps2(new byte[] {(byte) 0x80}));
            ElfFile f = ElfFile.from(image);

            // Re-reading the same failing section keeps failing on the same instance instead of
            // caching a partial result, and never disturbs neighboring sections.
            for (int attempt = 0; attempt < 2; attempt++) {
                try {
                    f.getSection(Fixture.badSectionIndex());
                    c.check(false, "same-instance read should fail");
                } catch (ElfException e) {
                    c.check(e.getMessage().contains(".bad.packed") && e.getMessage().contains("byte "),
                            "same-instance failure keeps name and position");
                }
            }
            // Neighboring sections remain usable while the bad section keeps failing.
            ElfRelocationAddendSection good =
                    (ElfRelocationAddendSection) f.getSection(Fixture.goodSectionIndex());
            c.check(good.relocations.length == 1, "good packed section readable");
            ElfRelocationAddendSection ordinary = (ElfRelocationAddendSection) f.getSection(4);
            c.check(ordinary.relocations.length == 1, "ordinary section readable");
            c.check(good.relocations[0].r_offset == 0x108, "good section survives repeated bad reads");

            // A fresh parser over the same bytes is unaffected.
            ElfFile reparsed = ElfFile.from(image.clone());
            ElfRelocationAddendSection reGood =
                    (ElfRelocationAddendSection) reparsed.getSection(Fixture.goodSectionIndex());
            c.check(reGood.relocations.length == 1, "fresh parse of neighbor section");
            expectFailure(c, image, "fresh parse still reports the bad section");
        }
    }

    // ------------------------------------------------------------------
    // ownership
    // ------------------------------------------------------------------

    private static void shLinkSymbolTables(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            Fixture.Mixed m = Fixture.mixed(wide, ByteOrder.LITTLE_ENDIAN);
            ElfFile f = ElfFile.from(m.bytes);
            ElfSymbolTableSection symtab = (ElfSymbolTableSection) f.getSection(m.symtab);
            ElfSymbolTableSection dynsym = (ElfSymbolTableSection) f.getSection(m.dynsym);

            ElfRelocationAddendSection packedRela =
                    (ElfRelocationAddendSection) f.getSection(m.packedRela);
            for (ElfRelocationAddend r : packedRela.relocations) {
                c.check(r.getSymbol() == dynsym.symbols[r.getSymbolIndex()], "packed RELA uses sh_link dynsym");
            }
            ElfRelocationSection packedRel = (ElfRelocationSection) f.getSection(m.packedRel);
            for (ElfRelocation r : packedRel.relocations) {
                c.check(r.getSymbol() == dynsym.symbols[r.getSymbolIndex()], "packed REL uses sh_link dynsym");
            }
            ElfRelocationAddendSection ordinary =
                    (ElfRelocationAddendSection) f.getSection(m.ordinaryRela);
            for (ElfRelocationAddend r : ordinary.relocations) {
                c.check(r.getSymbol() == symtab.symbols[r.getSymbolIndex()], "ordinary RELA uses sh_link symtab");
            }
            ElfRelocationSection ordinaryRel = (ElfRelocationSection) f.getSection(m.ordinaryRel);
            for (ElfRelocation r : ordinaryRel.relocations) {
                c.check(r.getSymbol() == symtab.symbols[r.getSymbolIndex()], "ordinary REL uses sh_link symtab");
            }
        }
    }

    private static void sameIndexDifferentNames(TaskCheck.Check c) {
        Fixture.Mixed m = Fixture.mixed(true, ByteOrder.LITTLE_ENDIAN);
        ElfFile f = ElfFile.from(m.bytes);
        ElfSymbolTableSection symtab = (ElfSymbolTableSection) f.getSection(m.symtab);
        ElfSymbolTableSection dynsym = (ElfSymbolTableSection) f.getSection(m.dynsym);

        c.check("alpha".equals(symtab.symbols[1].getName()), "symtab[1] name");
        c.check("gamma".equals(dynsym.symbols[1].getName()), "dynsym[1] name");
        c.check(symtab.symbols[1] != dynsym.symbols[1], "same index is a different object per table");

        ElfRelocationAddend r = ((ElfRelocationAddendSection) f.getSection(m.packedRela)).relocations[0];
        c.check(r.getSymbolIndex() == 1, "packed record references index 1");
        c.check(r.getSymbol() == dynsym.symbols[1], "packed record resolves into dynsym");
        c.check("gamma".equals(r.getSymbol().getName()), "packed record gets the dynsym name");
    }

    private static void stringTableByLink(TaskCheck.Check c) {
        // Neither conventional name (.strtab/.dynstr) exists; resolution must follow sh_link only.
        Fixture.Image img = new Fixture.Image(false, ByteOrder.LITTLE_ENDIAN);
        int customSymStrings = img.add(".strtable.custom", 3, Fixture.strings("alpha"), 0, 0, 0);
        int customDynStrings = img.add(".dynstr.renamed", 3, Fixture.strings("gamma"), 0, 0, 0);
        int symtab = img.add(".symtab", 2,
                img.symbols(new long[][] {{0, 0}, {1, 0x1000}}), customSymStrings, 1, 16);
        int dynsym = img.add(".dynsym", 11,
                img.symbols(new long[][] {{0, 0}, {1, 0x2000}}), customDynStrings, 1, 16);
        byte[] packed = Fixture.packed(false, 1, 0x10, 1, 0x3, 0x8, Fixture.info(false, 1, 2));
        int packedIndex = img.add(".android.rela", Fixture.ANDROID_RELA, packed, dynsym, 0, 1);

        ElfFile f = ElfFile.from(img.bytes());
        c.check(f.getStringTable() == null, "no conventionally named .strtab exists");
        c.check(f.getDynamicStringTable() == null, "no conventionally named .dynstr exists");
        ElfSymbolTableSection parsedSymtab = (ElfSymbolTableSection) f.getSection(symtab);
        ElfSymbolTableSection parsedDynsym = (ElfSymbolTableSection) f.getSection(dynsym);
        c.check("alpha".equals(parsedSymtab.symbols[1].getName()), "symtab name via sh_link");
        c.check("gamma".equals(parsedDynsym.symbols[1].getName()), "dynsym name via sh_link");

        ElfRelocationAddend r =
                ((ElfRelocationAddendSection) f.getSection(packedIndex)).relocations[0];
        c.check("gamma".equals(r.getSymbol().getName()), "packed record follows the dynsym link");
    }

    private static void sameObjectIdentity(TaskCheck.Check c) {
        Fixture.Mixed m = Fixture.mixed(false, ByteOrder.BIG_ENDIAN);
        ElfFile f = ElfFile.from(m.bytes);
        ElfSection first = f.getSection(m.packedRela);
        ElfSection second = f.getSection(m.packedRela);
        c.check(first == second, "memoized packed section instance");
        ElfRelocationAddend[] a = ((ElfRelocationAddendSection) first).relocations;
        ElfRelocationAddend[] b = ((ElfRelocationAddendSection) second).relocations;
        c.check(a == b, "same expanded array on repeated access");
        ElfSymbolTableSection dynsym = (ElfSymbolTableSection) f.getSection(m.dynsym);
        for (ElfRelocationAddend r : a) {
            c.check(r.getSymbol() == dynsym.symbols[r.getSymbolIndex()], "symbol identity stable");
        }
    }

    private static void invalidShLink(TaskCheck.Check c) {
        // A packed section whose sh_link is not a symbol table, and one whose sh_link is out of range.
        Fixture.Image wrongType = new Fixture.Image(false, ByteOrder.LITTLE_ENDIAN);
        int text = wrongType.add(".text", 1, new byte[16], 0, 0, 0);
        int wrongIndex = wrongType.add(".android.rela", Fixture.ANDROID_RELA,
                Fixture.packed(false, 1, 0x10, 1, 0x3, 0x8, Fixture.info(false, 1, 2)), text, 0, 1);
        ElfFile wrongFile = ElfFile.from(wrongType.bytes());
        try {
            ((ElfRelocationAddendSection) wrongFile.getSection(wrongIndex)).relocations[0].getSymbol();
            c.check(false, "non-symbol-table sh_link should fail");
        } catch (ElfException e) {
            c.check(e.getMessage().contains("sh_link"), "wrong-type link reported via ElfException");
        }

        Fixture.Image outOfRange = new Fixture.Image(false, ByteOrder.LITTLE_ENDIAN);
        outOfRange.add(".text", 1, new byte[16], 0, 0, 0);
        int orIndex = outOfRange.add(".android.rela", Fixture.ANDROID_RELA,
                Fixture.packed(false, 1, 0x10, 1, 0x3, 0x8, Fixture.info(false, 1, 2)), 999, 0, 1);
        ElfFile orFile = ElfFile.from(outOfRange.bytes());
        try {
            ((ElfRelocationAddendSection) orFile.getSection(orIndex)).relocations[0].getSymbol();
            c.check(false, "out-of-range sh_link should fail");
        } catch (ElfException e) {
            c.check(e.getMessage().contains("outside the section header table"),
                    "out-of-range link reported via ElfException");
        }
    }

    // ------------------------------------------------------------------
    // compatibility
    // ------------------------------------------------------------------

    private static void ordinaryUnchanged(TaskCheck.Check c) {
        // The shipped baseline fixture keeps its ordinary RELA semantics and gains packed support.
        ElfFile f = ElfFile.from(Fixture.basic());
        ElfRelocationAddendSection ordinary = (ElfRelocationAddendSection) f.getSection(5);
        c.check(ordinary.header.sh_type == ElfSectionHeader.SHT_RELA, "ordinary sh_type unchanged");
        ElfRelocationAddend r = ordinary.relocations[0];
        c.check(r.r_offset == 8 && r.r_addend == -4, "ordinary RELA fields unchanged");
        c.check("alpha".equals(r.getSymbol().getName()), "ordinary RELA symbol unchanged");

        ElfSection packed = f.getSection(6);
        c.check(packed instanceof ElfRelocationAddendSection, "packed section class");
        ElfRelocationAddend[] expanded = ((ElfRelocationAddendSection) packed).relocations;
        c.check(expanded.length == 2, "packed baseline count");
        for (ElfRelocationAddend pr : expanded) {
            c.check(pr.r_offset == 8, "packed baseline offset");
            c.check(pr.r_info == ((1L << 32) | 2), "packed baseline info");
            c.check(pr.r_addend == 0, "packed baseline addend");
            c.check("alpha".equals(pr.getSymbol().getName()), "packed baseline symbol");
        }

        Fixture.Mixed m = Fixture.mixed(true, ByteOrder.LITTLE_ENDIAN);
        ElfFile mixed = ElfFile.from(m.bytes);
        ElfRelocationSection rel = (ElfRelocationSection) mixed.getSection(m.ordinaryRel);
        c.check(rel.relocations.length == 2, "ordinary REL count");
        c.check(rel.relocations[0].r_offset == 0x70, "ordinary REL offset");
        c.check(rel.relocations[0].getSymbol() == ((ElfSymbolTableSection) mixed.getSection(m.symtab)).symbols[1],
                "ordinary REL symbol ownership");
    }

    private static void sectionsOfType(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            Fixture.Mixed m = Fixture.mixed(wide, ByteOrder.LITTLE_ENDIAN);
            ElfFile f = ElfFile.from(m.bytes);

            List<ElfRelocationSection> rels = f.sectionsOfType(ElfRelocationSection.class);
            c.check(rels.size() == 2, "class lookup finds ordinary + packed REL");
            c.check(rels.stream().anyMatch(s -> s.header.sh_type == Fixture.ANDROID_REL), "packed REL by class");
            c.check(rels.stream().anyMatch(s -> s.header.sh_type == ElfSectionHeader.SHT_REL), "ordinary REL by class");

            List<ElfRelocationAddendSection> relas = f.sectionsOfType(ElfRelocationAddendSection.class);
            c.check(relas.size() == 2, "class lookup finds ordinary + packed RELA");
            c.check(relas.stream().anyMatch(s -> s.header.sh_type == Fixture.ANDROID_RELA), "packed RELA by class");
            c.check(relas.stream().anyMatch(s -> s.header.sh_type == ElfSectionHeader.SHT_RELA), "ordinary RELA by class");

            c.check(f.sectionsOfType(Fixture.ANDROID_REL).size() == 1, "integer type lookup REL");
            c.check(f.sectionsOfType(Fixture.ANDROID_RELA).size() == 1, "integer type lookup RELA");
            c.check(f.firstSectionByType(ElfRelocationSection.class) != null, "firstSectionByType class lookup");
        }
    }

    private static void interleavedReads(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            for (ByteOrder order : ORDERS) {
                Fixture.Mixed m = Fixture.mixed(wide, order);
                byte[] rawPackedRela = ElfFile.from(m.bytes).getSection(m.packedRela).getData();

                long[][] snapshot = null;
                for (int pass = 0; pass < 3; pass++) {
                    ElfFile f = ElfFile.from(m.bytes);
                    // Interleave packed relocation, symbol table, ordinary section and raw byte reads.
                    ElfRelocationAddendSection packedRela =
                            (ElfRelocationAddendSection) f.getSection(m.packedRela);
                    ElfSymbolTableSection dynsym = (ElfSymbolTableSection) f.getSection(m.dynsym);
                    ElfRelocationSection packedRel = (ElfRelocationSection) f.getSection(m.packedRel);
                    ElfSymbolTableSection symtab = (ElfSymbolTableSection) f.getSection(m.symtab);
                    ElfSection raw = f.getSection(m.packedRela);

                    long[][] now = new long[packedRela.relocations.length][];
                    for (int i = 0; i < now.length; i++) {
                        ElfRelocationAddend r = packedRela.relocations[i];
                        now[i] = new long[] {r.r_offset, r.r_info, r.r_addend};
                        c.check(dynsym.symbols[r.getSymbolIndex()] == r.getSymbol(), "interleaved symbol identity");
                    }
                    if (snapshot == null) {
                        snapshot = now;
                    } else {
                        c.check(Arrays.deepEquals(snapshot, now), "consistent expansion across passes");
                    }
                    c.check(Arrays.equals(raw.getData(), rawPackedRela), "raw bytes consistent");
                    c.check(packedRel.relocations.length == 4, "packed REL readable between other reads");
                    c.check(symtab.symbols.length == 3, "symtab readable between other reads");
                }
            }
        }
    }

    private static void rawViewPreserved(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            Fixture.Mixed m = Fixture.mixed(wide, ByteOrder.LITTLE_ENDIAN);
            byte[] original = m.bytes.clone();
            ElfFile f = ElfFile.from(m.bytes);

            ElfSection packedRela = f.getSection(m.packedRela);
            ElfSection packedRel = f.getSection(m.packedRel);
            c.check(packedRela.header.sh_type == Fixture.ANDROID_RELA, "RELA sh_type stays on-disk");
            c.check(packedRel.header.sh_type == Fixture.ANDROID_REL, "REL sh_type stays on-disk");

            // Force full expansion, then confirm the compressed bytes and the whole image are unchanged.
            c.check(((ElfRelocationAddendSection) packedRela).relocations.length == 6, "expand RELA");
            c.check(((ElfRelocationSection) packedRel).relocations.length == 4, "expand REL");
            byte[] expectedRela = Fixture.mixed(wide, ByteOrder.LITTLE_ENDIAN).bytes;
            ElfFile reprobe = ElfFile.from(expectedRela);
            c.check(Arrays.equals(packedRela.getData(),
                    reprobe.getSection(m.packedRela).getData()), "RELA getData returns APS2 bytes");
            c.check(Arrays.equals(m.bytes, original), "image not rewritten into plain relocations");
            c.check(packedRela.getData()[0] == 'A' && packedRela.getData()[3] == '2', "APS2 magic intact");
        }
    }

    private static void relRejectsAddends(TaskCheck.Check c) {
        for (boolean wide : WIDTHS) {
            // HAS_ADDEND is illegal in SHT_ANDROID_REL: the decoder must reject it rather than
            // reading target bytes to infer an addend.
            byte[] image = Fixture.withBadSection(wide, ByteOrder.LITTLE_ENDIAN, Fixture.ANDROID_REL,
                    Fixture.packed(wide, 1, 0, 1, 0x8, 0, Fixture.info(wide, 1, 2), 0));
            expectFailure(c, image, "REL stream with HAS_ADDEND rejected");

            // A valid REL stream expands through the addend-free ElfRelocation model.
            Fixture.Mixed m = Fixture.mixed(wide, ByteOrder.LITTLE_ENDIAN);
            ElfFile f = ElfFile.from(m.bytes);
            ElfRelocationSection rel = (ElfRelocationSection) f.getSection(m.packedRel);
            for (ElfRelocation r : rel.relocations) {
                c.check(r.getClass() == ElfRelocation.class, "REL uses plain ElfRelocation");
                c.check(r.getSymbol() != null, "REL symbol resolvable");
            }
        }
    }

    // ------------------------------------------------------------------
    // helpers
    // ------------------------------------------------------------------

    private static void expectFailure(TaskCheck.Check c, byte[] image, String label) {
        ElfFile f = ElfFile.from(image);
        try {
            f.getSection(Fixture.badSectionIndex());
            c.check(false, label + ": expected ElfException");
        } catch (ElfException e) {
            c.check(e.getMessage().contains(".bad.packed"), label + ": section name in " + e.getMessage());
            c.check(e.getMessage().contains("byte "), label + ": byte position in " + e.getMessage());
        }
    }
}
