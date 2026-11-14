/*
 * Copyright (c) 2009-2012 Panxiaobo
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.googlecode.d2j.reader;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Read-only support for the synthetic CompactDex v001 container described by
 * Android ART's {@code libdexfile/dex/compact_dex_file.h} (Header, CodeItem) and
 * {@code compact_offset_table.h} (Accessor).
 *
 * <p>
 * The ID tables live in the main file region while every payload (string data, type lists,
 * class data, code items, debug data, map data) lives in the owned data region beginning at
 * {@code data_off}; offsets stored in the file address that region. A compact code item is a
 * two-uint16 header preceded by a preheader whose extension words (tries size, outs size, the
 * two-word instruction count extension, then the signed register delta adjacent to the header)
 * only exist for fields flagged in the lower five bits; the item carries no debug offset, so the
 * debug info offset of a method is recovered, per method id, from the sparse table held in the
 * file header.
 * </p>
 *
 * Nothing here loads or executes the image; all access is plain byte reading.
 */
/* package */final class CompactDexSupport {

    /** First byte sequence of a CompactDex v001 image, {@code "cdex001\0"}. */
    static final byte[] MAGIC = { 'c', 'd', 'e', 'x', '0', '0', '1', 0 };

    /** Size of the CompactDex v001 header (the ordinary 0x70 header plus six uint32 fields). */
    static final int HEADER_SIZE = 0x88;

    // Preheader flags, lower five bits of the second code item uint16.
    private static final int PREHEADER_FLAG_REGISTER_DELTA = 1 << 3;
    private static final int PREHEADER_HAS_INSTRUCTION_SIZE_EXTENSION = 1 << 2;
    private static final int PREHEADER_HAS_OUTS_SIZE_EXTENSION = 1 << 1;
    private static final int PREHEADER_HAS_TRIES_SIZE_EXTENSION = 1 << 0;
    private static final int PREHEADER_INSTRUCTIONS_SIZE_MASK = 0xFFE0;
    private static final int PREHEADER_INSTRUCTIONS_SIZE_SHIFT = 5;
    private static final int PREHEADER_FLAGS_MASK = 0x1F;

    /** One lookup entry per that many method ids in the sparse debug offset table. */
    static final int ENTRIES_PER_ENTRY = 16;

    private CompactDexSupport() {
    }

    static boolean isCompactDex(byte[] data) {
        if (data == null || data.length < MAGIC.length) {
            return false;
        }
        for (int i = 0; i < MAGIC.length; i++) {
            if (data[i] != MAGIC[i]) {
                return false;
            }
        }
        return true;
    }

    /**
     * Header information of a CompactDex v001 image. All offsets stored here are relative to the
     * owned data region, matching the data-region {@link ByteBuffer} view used while reading
     * payload items.
     */
    /* package */static final class Header {
        final int featureFlags;
        /** Data-relative position of the sparse debug info offsets table. */
        final int debugInfoOffsetsPos;
        /** Data-relative position of the per-16-method lookup table. */
        final int debugInfoOffsetsTableOff;
        /** Running offset each sparse block starts from, relative to the data region. */
        final int debugInfoBase;
        final int ownedDataBegin;
        final int ownedDataEnd;

        Header(ByteBuffer in) {
            this.featureFlags = in.getInt(0x70);
            this.debugInfoOffsetsPos = in.getInt(0x74);
            this.debugInfoOffsetsTableOff = this.debugInfoOffsetsPos + in.getInt(0x78);
            this.debugInfoBase = in.getInt(0x7C);
            this.ownedDataBegin = in.getInt(0x80);
            this.ownedDataEnd = in.getInt(0x84);
        }
    }

    /**
     * Decoded view of a compact code item. The item is only read, never copied into an expanded
     * dex layout.
     */
    /* package */static final class CodeItem {
        /** Total registers, local registers plus incoming parameter words. */
        final int registersSize;
        final int insSize;
        final int outsSize;
        final int triesSize;
        /** Number of 16-bit code units of the instruction stream. */
        final int insnsSize;
        /** Data-relative position of the first instruction unit. */
        final int insnsOff;
        /** Data-relative position immediately after the instruction stream (try items begin here). */
        final int afterInsnsOff;

        CodeItem(int registersSize, int insSize, int outsSize, int triesSize, int insnsSize,
                int insnsOff, int afterInsnsOff) {
            this.registersSize = registersSize;
            this.insSize = insSize;
            this.outsSize = outsSize;
            this.triesSize = triesSize;
            this.insnsSize = insnsSize;
            this.insnsOff = insnsOff;
            this.afterInsnsOff = afterInsnsOff;
        }
    }

    /**
     * Parse the compact code item whose two-word header starts at {@code codeOff}. Extension
     * words sit directly before the header, laid out in forward field order: tries size, outs
     * size, the two-word instruction count extension (low then high uint16), then the signed
     * register delta which is immediately adjacent to the header.
     *
     * <p>
     * The first uint16 packs four nibbles: local registers, incoming parameter words, outgoing
     * call words and try count. The total register count already includes the incoming words.
     * </p>
     */
    static CodeItem readCodeItem(ByteBuffer in, int codeOff) {
        int first = in.getShort(codeOff) & 0xFFFF;
        int second = in.getShort(codeOff + 2) & 0xFFFF;

        int localRegisters = (first >> 12) & 0xF;
        int insSize = (first >> 8) & 0xF;
        int outsSize = (first >> 4) & 0xF;
        int triesSize = first & 0xF;
        int insnsSize = (second & PREHEADER_INSTRUCTIONS_SIZE_MASK) >>> PREHEADER_INSTRUCTIONS_SIZE_SHIFT;
        int flags = second & PREHEADER_FLAGS_MASK;

        boolean hasTriesExt = (flags & PREHEADER_HAS_TRIES_SIZE_EXTENSION) != 0;
        boolean hasOutsExt = (flags & PREHEADER_HAS_OUTS_SIZE_EXTENSION) != 0;
        boolean hasInsnsExt = (flags & PREHEADER_HAS_INSTRUCTION_SIZE_EXTENSION) != 0;
        boolean hasRegExt = (flags & PREHEADER_FLAG_REGISTER_DELTA) != 0;
        // The instruction-count extension occupies two words, every other extension one.
        int preHeaderWords = (hasTriesExt ? 1 : 0) + (hasOutsExt ? 1 : 0) + (hasInsnsExt ? 2 : 0)
                + (hasRegExt ? 1 : 0);
        int cursor = codeOff - preHeaderWords * 2;
        if (hasTriesExt) {
            triesSize += in.getShort(cursor) & 0xFFFF;
            cursor += 2;
        }
        if (hasOutsExt) {
            outsSize += in.getShort(cursor) & 0xFFFF;
            cursor += 2;
        }
        if (hasInsnsExt) {
            // Little-endian 32-bit high part split into a low then a high uint16.
            int hi = (in.getShort(cursor) & 0xFFFF) | ((in.getShort(cursor + 2) & 0xFFFF) << 16);
            insnsSize += hi;
            cursor += 4;
        }
        if (hasRegExt) {
            // Signed delta applied to the local-register nibble.
            localRegisters += in.getShort(cursor);
        }
        int registersSize = localRegisters + insSize;

        int insnsOff = codeOff + 4;
        return new CodeItem(registersSize, insSize, outsSize, triesSize, insnsSize, insnsOff,
                insnsOff + insnsSize * 2);
    }

    /**
     * Resolve the debug info offset (relative to the owned data region) of method
     * {@code methodIndex} through the sparse debug offset table, or {@code 0} when the method
     * has no debug info. The returned offset feeds the ordinary debug info visitor unchanged.
     *
     * <p>
     * Each 16-method block begins with a big-endian 16-bit presence mask followed by unsigned
     * LEB128 deltas; a block's running offset starts at {@code debug_info_base}. The per-block
     * lookup entries are little-endian uint32 offsets, relative to
     * {@code debug_info_offsets_pos}, pointing at the block start.
     * </p>
     *
     * @param in
     *            data-region view (positions are data-relative)
     */
    static int findDebugInfoOffset(ByteBuffer in, Header header, int methodIndex) {
        if (header.debugInfoOffsetsPos <= 0) {
            // No sparse table in this image: the method genuinely has no debug info.
            return 0;
        }
        int blockIndex = methodIndex / ENTRIES_PER_ENTRY;
        int regionSize = in.capacity();
        int lookupEntryOff = header.debugInfoOffsetsTableOff + blockIndex * 4;
        if (lookupEntryOff < 0 || lookupEntryOff + 4 > regionSize) {
            return 0;
        }
        int blockOff = header.debugInfoOffsetsPos + in.getInt(lookupEntryOff);
        if (blockOff < 0 || blockOff + 2 > regionSize) {
            return 0;
        }
        // Big-endian presence mask even though the container is otherwise little endian. The
        // mask is a 16-bit value whose bit N marks the method at slot N within the block.
        int presence = ((in.get(blockOff) & 0xFF) << 8) | (in.get(blockOff + 1) & 0xFF);
        int slot = methodIndex % ENTRIES_PER_ENTRY;
        if ((presence & (1 << slot)) == 0) {
            return 0;
        }

        ByteBuffer block = in.duplicate().order(ByteOrder.LITTLE_ENDIAN);
        block.position(blockOff + 2);
        int running = header.debugInfoBase;
        for (int i = 0; i <= slot; i++) {
            if ((presence & (1 << i)) != 0) {
                running += readULeb128(block);
            }
        }
        return running;
    }

    private static int readULeb128(ByteBuffer in) {
        int value = 0;
        int count = 0;
        int b = in.get();
        while ((b & 0x80) != 0) {
            value |= (b & 0x7f) << count;
            count += 7;
            b = in.get();
        }
        value |= (b & 0x7f) << count;
        return value;
    }
}
