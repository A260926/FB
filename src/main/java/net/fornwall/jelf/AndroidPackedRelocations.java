package net.fornwall.jelf;

/**
 * Decoder for the Android {@code APS2} packed relocation wire format used by {@link
 * ElfSectionHeader#SHT_ANDROID_REL} and {@link ElfSectionHeader#SHT_ANDROID_RELA} sections.
 *
 * <p>The format is a literal {@code APS2} magic followed by SLEB128 integers. See {@code
 * task_support/FORMAT.md} for the grouping contract. SLEB128 byte order is independent of the ELF
 * byte order. All failures are reported as {@link ElfException} carrying the section name and the
 * byte position within the compressed data.
 */
final class AndroidPackedRelocations {

    private static final long FLAG_INFO = 1;
    private static final long FLAG_OFFSET_DELTA = 2;
    private static final long FLAG_ADDEND = 4;
    private static final long FLAG_HAS_ADDEND = 8;
    private static final long KNOWN_FLAGS = FLAG_INFO | FLAG_OFFSET_DELTA | FLAG_ADDEND | FLAG_HAS_ADDEND;

    /**
     * Upper bound on the number of declared relocations. A record can consume zero bytes when all
     * fields are grouped, so the stream length alone does not bound the count; this guard keeps a
     * hostile header from exhausting memory while remaining far above real shared libraries.
     */
    private static final long MAX_RELOCATIONS = 4L * 1024 * 1024;

    /** The decoded relocation fields, in stream order. */
    static final class Result {
        final long[] offsets;
        final long[] infos;
        final long[] addends;

        Result(long[] offsets, long[] infos, long[] addends) {
            this.offsets = offsets;
            this.infos = infos;
            this.addends = addends;
        }
    }

    private final String sectionName;
    private final int width;
    private final byte[] data;
    /** Current read cursor, an offset into {@link #data}. */
    private int position;

    private AndroidPackedRelocations(String sectionName, int width, byte[] data) {
        this.sectionName = sectionName;
        this.width = width;
        this.data = data;
    }

    /**
     * Decodes a packed relocation section.
     *
     * @param sectionName name used in error messages
     * @param width ELF file word width in bits, 32 or 64
     * @param data the raw on-disk section bytes
     * @param withAddend whether the owning section is an {@code SHT_ANDROID_RELA} section
     */
    static Result decode(String sectionName, int width, byte[] data, boolean withAddend) {
        return new AndroidPackedRelocations(sectionName, width, data).decode(withAddend);
    }

    /**
     * Returns the name of a section for error reporting, or a placeholder if the name cannot be
     * resolved (for example when the section name string table itself is unreadable).
     */
    static String sectionName(ElfSectionHeader header) {
        try {
            String name = header.getName();
            return name != null ? name : "<unnamed>";
        } catch (ElfException e) {
            return "<unnamed>";
        }
    }

    private Result decode(boolean withAddend) {
        if (data.length < 4) {
            throw errorAt(0, "truncated APS2 header");
        }
        if (data[0] != 'A' || data[1] != 'P' || data[2] != 'S' || data[3] != '2') {
            throw errorAt(0, "invalid APS2 magic");
        }
        position = 4;

        int countStart = position;
        long relocationCount = readSleb();
        if (relocationCount < 0) {
            throw errorAt(countStart, "negative relocation count " + relocationCount);
        }
        if (relocationCount > MAX_RELOCATIONS) {
            throw errorAt(countStart, "relocation count too large: " + relocationCount);
        }
        long initialOffset = unsignedBits(readSleb());

        int count = (int) relocationCount;
        long[] offsets = new long[count];
        long[] infos = new long[count];
        long[] addends = withAddend ? new long[count] : null;

        long offset = initialOffset;
        long addend = 0;
        int produced = 0;

        while (produced < count) {
            int groupStart = position;
            long groupSize = readSleb();
            if (groupSize <= 0) {
                throw errorAt(groupStart, "group_size must be positive, was " + groupSize);
            }
            int flagsStart = position;
            long flags = readSleb();
            if (flags < 0 || (flags & ~KNOWN_FLAGS) != 0) {
                throw errorAt(flagsStart, "unsupported group flags 0x" + Long.toHexString(flags));
            }
            if (!withAddend && (flags & (FLAG_ADDEND | FLAG_HAS_ADDEND)) != 0) {
                throw errorAt(flagsStart, "addend flags are not allowed in an SHT_ANDROID_REL section");
            }
            if ((flags & FLAG_ADDEND) != 0 && (flags & FLAG_HAS_ADDEND) == 0) {
                throw errorAt(flagsStart, "ADDEND flag used without HAS_ADDEND");
            }
            if (groupSize > count - produced) {
                throw errorAt(groupStart, "group_size " + groupSize + " exceeds the declared relocation count "
                        + count + " (" + produced + " already produced)");
            }

            boolean perRecordInfo = (flags & FLAG_INFO) == 0;
            boolean perRecordOffset = (flags & FLAG_OFFSET_DELTA) == 0;
            boolean groupedAddend = (flags & FLAG_ADDEND) != 0;
            boolean hasAddend = (flags & FLAG_HAS_ADDEND) != 0;

            long info = 0;
            long offsetDelta = 0;
            int offsetDeltaStart = flagsStart;
            if (!perRecordOffset) {
                offsetDeltaStart = position;
                offsetDelta = readSleb();
            }
            if (!perRecordInfo) {
                info = unsignedBits(readSleb());
            }
            if (hasAddend) {
                if (groupedAddend) {
                    // The shared addend delta is applied once, before the group's records.
                    int start = position;
                    addend = addAddend(addend, readSleb(), start);
                }
            } else {
                // A group without the HAS_ADDEND flag resets the running addend.
                addend = 0;
            }

            for (long i = 0; i < groupSize; i++) {
                if (perRecordOffset) {
                    offsetDeltaStart = position;
                    offsetDelta = readSleb();
                }
                offset = addOffset(offset, offsetDelta, offsetDeltaStart);

                if (perRecordInfo) {
                    info = unsignedBits(readSleb());
                }

                long recordAddend = 0;
                if (hasAddend) {
                    if (!groupedAddend) {
                        int start = position;
                        addend = addAddend(addend, readSleb(), start);
                    }
                    recordAddend = addend;
                }

                offsets[produced] = offset;
                infos[produced] = info;
                if (withAddend) {
                    addends[produced] = recordAddend;
                }
                produced++;
            }
        }

        if (position != data.length) {
            throw errorAt(position, (data.length - position) + " trailing byte(s) after the declared relocation count");
        }
        return new Result(offsets, infos, addends);
    }

    /** Reads a signed LEB128 integer bounded by the ELF file word width. */
    private long readSleb() {
        return readSlebAt(position);
    }

    private long readSlebAt(int start) {
        int maxBytes = (width + 6) / 7;
        long result = 0;
        int shift = 0;
        for (int i = 0; ; i++) {
            if (i >= maxBytes) {
                throw errorAt(start, "SLEB128 integer is wider than " + maxBytes + " bytes for a " + width
                        + "-bit file");
            }
            if (position >= data.length) {
                throw errorAt(start, "truncated SLEB128 integer");
            }
            int b = data[position++] & 0xFF;
            int low = b & 0x7F;
            if (i == maxBytes - 1) {
                // Only this many payload bits still fit in the file word; the rest must be
                // a valid sign extension of the highest payload bit.
                int payloadBits = width - 7 * i;
                int sign = (low >> (payloadBits - 1)) & 1;
                int expectedHigh = sign == 1 ? 0x7F >>> payloadBits : 0;
                if ((low >>> payloadBits) != expectedHigh) {
                    throw errorAt(start, "SLEB128 value does not fit in a signed " + width + "-bit integer");
                }
            }
            result |= (long) low << shift;
            shift += 7;
            if ((b & 0x80) == 0) {
                if (shift < width) {
                    if ((b & 0x40) != 0) {
                        result |= -1L << shift;
                    }
                } else {
                    long mask = width == 32 ? 0xFFFFFFFFL : -1L;
                    result &= mask;
                    if ((result & (1L << (width - 1))) != 0) {
                        result |= ~mask;
                    }
                }
                return result;
            }
        }
    }

    /** Reinterprets a decoded signed value as an unsigned file-width bit pattern. */
    private long unsignedBits(long value) {
        return width == 32 ? (value & 0xFFFFFFFFL) : value;
    }

    private long addOffset(long offset, long delta, int start) {
        if (width == 32) {
            long sum = offset + delta;
            if (sum < 0 || sum > 0xFFFFFFFFL) {
                throw errorAt(start, "offset arithmetic leaves the unsigned 32-bit range");
            }
            return sum;
        }
        if (delta >= 0) {
            // ~delta is 2^64 - 1 - delta for a non-negative delta.
            if (Long.compareUnsigned(offset, ~delta) > 0) {
                throw errorAt(start, "offset arithmetic leaves the unsigned 64-bit range");
            }
            return offset + delta;
        }
        long magnitude = -delta; // Two's complement magnitude; unsigned 2^63 for Long.MIN_VALUE.
        if (Long.compareUnsigned(offset, magnitude) < 0) {
            throw errorAt(start, "offset arithmetic leaves the unsigned 64-bit range");
        }
        return offset - magnitude;
    }

    private long addAddend(long addend, long delta, int start) {
        if (width == 32) {
            long sum = addend + delta;
            if (sum < Integer.MIN_VALUE || sum > Integer.MAX_VALUE) {
                throw errorAt(start, "addend arithmetic leaves the signed 32-bit range");
            }
            return sum;
        }
        try {
            return Math.addExact(addend, delta);
        } catch (ArithmeticException e) {
            throw errorAt(start, "addend arithmetic leaves the signed 64-bit range");
        }
    }

    private ElfException errorAt(int offset, String message) {
        return new ElfException(
                "Section '" + sectionName + "' packed relocations at byte " + offset + ": " + message);
    }
}
