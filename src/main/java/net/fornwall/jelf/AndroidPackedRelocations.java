package net.fornwall.jelf;

import java.util.ArrayList;
import java.util.List;

/**
 * Decoder for Android packed relocation sections (APS2), as used by
 * {@link ElfSectionHeader#SHT_ANDROID_REL} and {@link ElfSectionHeader#SHT_ANDROID_RELA}.
 *
 * <p>The wire format is a four byte {@code APS2} magic followed by signed LEB128 integers
 * (relocation_count, initial_offset and a sequence of groups). Decoding only explains the symbolic
 * references: nothing is mapped, relocated or executed.
 *
 * <p>On any malformed input an {@link ElfException} is thrown before any (partial) result is handed
 * out. Error messages carry the section name and the byte position inside the compressed data.
 */
final class AndroidPackedRelocations {

    static final byte[] MAGIC = {'A', 'P', 'S', '2'};

    static final int FLAG_INFO = 0x1;
    static final int FLAG_OFFSET_DELTA = 0x2;
    static final int FLAG_ADDEND = 0x4;
    static final int FLAG_HAS_ADDEND = 0x8;
    private static final int KNOWN_FLAGS =
            FLAG_INFO | FLAG_OFFSET_DELTA | FLAG_ADDEND | FLAG_HAS_ADDEND;

    /** One expanded relocation. {@code addend} is only meaningful for RELA streams. */
    static final class Entry {
        final long offset;
        final long info;
        final long addend;

        Entry(long offset, long info, long addend) {
            this.offset = offset;
            this.info = info;
            this.addend = addend;
        }
    }

    private final byte[] data;
    private final String sectionName;
    private final boolean wide;
    private final boolean rela;
    private final int width;
    private final long unsignedMask;
    private final long signedMin;
    private final long signedMax;
    private final int maxLebBytes;

    private int position;

    private AndroidPackedRelocations(byte[] data, String sectionName, boolean wide, boolean rela) {
        this.data = data;
        this.sectionName = sectionName;
        this.wide = wide;
        this.rela = rela;
        this.width = wide ? Long.SIZE : Integer.SIZE;
        this.unsignedMask = wide ? -1L : 0xffffffffL;
        this.signedMin = wide ? Long.MIN_VALUE : -(1L << (Integer.SIZE - 1));
        this.signedMax = wide ? Long.MAX_VALUE : ((1L << (Integer.SIZE - 1)) - 1);
        this.maxLebBytes = (width + 6) / 7;
    }

    static List<Entry> decode(byte[] data, ElfSectionHeader header, boolean wide) {
        boolean rela = header.sh_type == ElfSectionHeader.SHT_ANDROID_RELA;
        return new AndroidPackedRelocations(data, header.getName(), wide, rela).decode();
    }

    private List<Entry> decode() {
        if (data.length < MAGIC.length) {
            throw error("truncated APS2 magic");
        }
        for (byte magicByte : MAGIC) {
            if (data[position] != magicByte) {
                throw error("bad APS2 magic");
            }
            position++;
        }

        long relocationCount = readCount("relocation_count");
        if (relocationCount < 0 || relocationCount > Integer.MAX_VALUE) {
            throw error("invalid relocation_count " + relocationCount);
        }

        long runningOffset = readUnsignedWidth("initial_offset");
        long runningAddend = 0;
        long produced = 0;
        List<Entry> entries = new ArrayList<>((int) relocationCount);

        while (produced < relocationCount) {
            long groupSize = readCount("group_size");
            if (groupSize <= 0) {
                throw error("invalid group_size " + groupSize);
            }
            long flags = readCount("group_flags");
            if (flags < 0 || (flags & ~KNOWN_FLAGS) != 0) {
                throw error("invalid group_flags 0x" + Long.toHexString(flags));
            }
            boolean groupInfo = (flags & FLAG_INFO) != 0;
            boolean groupOffset = (flags & FLAG_OFFSET_DELTA) != 0;
            boolean groupAddend = (flags & FLAG_ADDEND) != 0;
            boolean hasAddend = (flags & FLAG_HAS_ADDEND) != 0;

            if (groupAddend && !hasAddend) {
                throw error("ADDEND flag without HAS_ADDEND");
            }
            if ((groupAddend || hasAddend) && !rela) {
                throw error("addend flags are not permitted in SHT_ANDROID_REL");
            }
            if (produced + groupSize > relocationCount) {
                throw error("group of " + groupSize + " records exceeds relocation_count " + relocationCount);
            }

            if (groupOffset) {
                runningOffset = addUnsignedWidth(runningOffset, readSignedWidth("offset_delta"));
            }
            long groupRInfo = 0;
            if (groupInfo) {
                groupRInfo = readUnsignedWidth("r_info");
            }
            if (hasAddend) {
                if (groupAddend) {
                    // The shared addend delta updates the running addend once, before the records.
                    runningAddend = addSignedWidth(runningAddend, readSignedWidth("addend_delta"));
                }
                // Bit 8 without bit 4: the running addend carries over and each record adds its own delta.
            } else {
                // A group without addends resets the running addend to zero.
                runningAddend = 0;
            }

            for (long record = 0; record < groupSize; record++) {
                if (!groupOffset) {
                    runningOffset = addUnsignedWidth(runningOffset, readSignedWidth("offset_delta"));
                }
                long rInfo = groupInfo ? groupRInfo : readUnsignedWidth("r_info");
                if (hasAddend && !groupAddend) {
                    runningAddend = addSignedWidth(runningAddend, readSignedWidth("addend_delta"));
                }
                entries.add(new Entry(runningOffset, rInfo, rela ? runningAddend : 0));
                produced++;
            }
        }

        if (position != data.length) {
            throw error("trailing " + (data.length - position) + " byte(s) after " + produced + " relocation(s)");
        }
        return entries;
    }

    /** Counts and flags are non-negative SLEB128 integers. */
    private long readCount(String what) {
        int start = position;
        long value = readSleb(what);
        if (value < 0) {
            throw error(what + " must be non-negative", start);
        }
        return value;
    }

    /**
     * Offset and r_info are unsigned file-width bit patterns carried through the corresponding signed
     * SLEB128 representation.
     */
    private long readUnsignedWidth(String what) {
        int start = position;
        long value = readSleb(what);
        return coerceUnsigned(value, what, start);
    }

    private long readSignedWidth(String what) {
        int start = position;
        long value = readSleb(what);
        if (wide) {
            return value;
        } else if (value < signedMin || value > signedMax) {
            throw error(what + " " + value + " outside signed " + width + "-bit range", start);
        }
        return value;
    }

    private long coerceUnsigned(long value, String what, int start) {
        long result = value & unsignedMask;
        if (wide) {
            return result;
        }
        // A 32-bit pattern must round-trip through its signed representation.
        long signedView = (int) result;
        if (signedView != value) {
            throw error(what + " " + value + " outside unsigned " + width + "-bit range", start);
        }
        return result;
    }

    private long addUnsignedWidth(long current, long delta) {
        long result;
        if (wide) {
            result = current + delta;
            // The mathematical result must stay inside the unsigned 64-bit range: a positive delta
            // that wraps past 2^64-1 or a negative delta that wraps below 0 is an error.
            boolean wrapped = delta >= 0
                    ? Long.compareUnsigned(result, current) < 0
                    : Long.compareUnsigned(result, current) > 0;
            if (wrapped) {
                throw error("offset arithmetic outside unsigned " + width + "-bit range", position);
            }
        } else {
            long math = current + delta;
            if (math < 0L || math > 0xffffffffL) {
                throw error("offset " + math + " outside unsigned " + width + "-bit range", position);
            }
            result = math;
        }
        return result;
    }

    private long addSignedWidth(long current, long delta) {
        long result;
        if (wide) {
            try {
                result = Math.addExact(current, delta);
            } catch (ArithmeticException e) {
                throw error("addend arithmetic overflow", position);
            }
        } else {
            long math = current + delta;
            if (math < signedMin || math > signedMax) {
                throw error("addend " + math + " outside signed " + width + "-bit range", position);
            }
            result = math;
        }
        return result;
    }

    /**
     * Reads one signed LEB128 integer. SLEB128 byte order is fixed and does not follow the ELF byte
     * order. At most {@code ceil(width/7)} bytes with valid sign extension are accepted.
     */
    private long readSleb(String what) {
        int start = position;
        long result = 0;
        int shift = 0;
        byte raw = 0;
        int byteIndex = 0;
        while (true) {
            if (position >= data.length) {
                throw error("unterminated SLEB128 for " + what, start);
            }
            if (byteIndex >= maxLebBytes) {
                throw error("SLEB128 for " + what + " wider than " + width + " bits", start);
            }
            raw = data[position++];
            byteIndex++;
            result |= (raw & 0x7fL) << shift;
            shift += 7;
            if ((raw & 0x80) == 0) {
                break;
            }
        }
        if (byteIndex == maxLebBytes) {
            // The final byte carries usedBits value bits; everything above must be valid sign
            // extension matching the sign bit, otherwise the integer is wider than the file width.
            int usedBits = width - ((maxLebBytes - 1) * 7);
            int payload = raw & 0x7f;
            int lowMask = (1 << usedBits) - 1;
            int signBit = 1 << (usedBits - 1);
            int highBits = payload & ~lowMask & 0x7f;
            int expectedHighBits = (payload & signBit) != 0 ? (0x7f & ~lowMask) : 0;
            if (highBits != expectedHighBits) {
                throw error("SLEB128 for " + what + " wider than " + width + " bits", start);
            }
            // The 64-bit case already occupies every long bit; the 32-bit case needs explicit
            // sign extension from the sign bit of the final payload byte.
            if (!wide && (payload & signBit) != 0) {
                result |= -(1L << width);
            }
        } else if (shift < Long.SIZE && (raw & 0x40) != 0) {
            result |= -(1L << shift);
        }
        return result;
    }

    private ElfException error(String message) {
        return error(message, position);
    }

    private ElfException error(String message, int bytePosition) {
        String name = sectionName != null ? sectionName : "<unnamed>";
        return new ElfException(
                "Android packed relocation section '" + name + "' at byte " + bytePosition + ": " + message);
    }
}
