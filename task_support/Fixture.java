package net.fornwall.jelf;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/** Writes small inert ELF fixtures. No decoding or relocation application. */
public final class Fixture {
    public static final int ANDROID_REL=0x60000001, ANDROID_RELA=0x60000002;
    public static byte[] packed(long... words) {
        return packed(true, words);
    }
    /** Prefixes the APS2 magic and encodes each word as a width-aware signed LEB128 integer. */
    public static byte[] packed(boolean wide, long... words) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        out.writeBytes(new byte[]{65,80,83,50});
        for(long v:words) out.writeBytes(leb(wide, v));
        return out.toByteArray();
    }
    /** Encodes one 64-bit signed LEB128 integer without any magic prefix. */
    public static byte[] leb(long v) {
        return leb(true, v);
    }
    /** Encodes one signed LEB128 integer in the given file width without any magic prefix. */
    public static byte[] leb(boolean wide, long v) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        boolean more;
        if(wide) {
            long x=v;
            do { int b=(int)(x&127); x>>=7;
                more=!((x==0&&(b&64)==0)||(x==-1&&(b&64)!=0));
                out.write(more ? b|128 : b);
            } while(more);
        } else {
            int x=(int)v;
            do { int b=x&127; x>>=7;
                more=!((x==0&&(b&64)==0)||(x==-1&&(b&64)!=0));
                out.write(more ? b|128 : b);
            } while(more);
        }
        return out.toByteArray();
    }
    /** Prefixes raw (possibly malformed) APS2 payload bytes with the four byte magic. */
    public static byte[] aps2(byte[] payload) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        out.writeBytes(new byte[]{65,80,83,50});
        out.writeBytes(payload);
        return out.toByteArray();
    }
    /** A NUL-prefixed string table holding the given names in order. */
    public static byte[] strings(String... names) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        out.write(0);
        for(String name:names) {
            out.writeBytes(name.getBytes(StandardCharsets.US_ASCII));
            out.write(0);
        }
        return out.toByteArray();
    }
    /** Encodes a symbol/type pair as an r_info bit pattern for the file width. */
    public static long info(boolean wide,int symbolIndex,int type) {
        return wide ? (((long) symbolIndex) << 32) | type : (symbolIndex << 8) | type;
    }
    public static final class Image {
        public final boolean wide; public final ByteOrder order;
        private final List<byte[]> data=new ArrayList<>();
        private final List<long[]> headers=new ArrayList<>();
        private final ByteArrayOutputStream names=new ByteArrayOutputStream();
        public Image(boolean wide, ByteOrder order) {
            this.wide=wide;this.order=order; names.write(0);
            add("",0,new byte[0],0,0,0);
            add(".shstrtab",3,new byte[0],0,0,0);
        }
        public int add(String name,int type,byte[] bytes,int link,int info,int entrySize) {
            int n=names.size(); names.writeBytes(name.getBytes(StandardCharsets.US_ASCII));names.write(0);
            headers.add(new long[]{n,type,0,0,0,bytes.length,link,info,1,entrySize});
            data.add(bytes.clone());return data.size()-1;
        }
        public byte[] symbol(int name,long value) {
            ByteBuffer b=ByteBuffer.allocate(wide?48:32).order(order);b.position(wide?24:16);
            b.putInt(name);
            if(wide)b.put((byte)0x12).put((byte)0).putShort((short)2).putLong(value).putLong(4);
            else b.putInt((int)value).putInt(4).put((byte)0x12).put((byte)0).putShort((short)2);
            return b.array();
        }
        /** Writes a symbol table: each row is {st_name offset, st_value}. */
        public byte[] symbols(long[][] rows) {
            int ent=wide?24:16;
            ByteBuffer b=ByteBuffer.allocate(ent*rows.length).order(order);
            for(int i=0;i<rows.length;i++) {
                b.position(i*ent);
                int name=(int)rows[i][0]; long value=rows[i][1];
                b.putInt(name);
                if(wide)b.put((byte)0x12).put((byte)0).putShort((short)2).putLong(value).putLong(4);
                else b.putInt((int)value).putInt(4).put((byte)0x12).put((byte)0).putShort((short)2);
            }
            return b.array();
        }
        public byte[] rel(long offset,long info) {
            ByteBuffer b=ByteBuffer.allocate(wide?16:8).order(order);
            if(wide)b.putLong(offset).putLong(info);
            else b.putInt((int)offset).putInt((int)info);
            return b.array();
        }
        public byte[] rela(long offset,long info,long addend) {
            ByteBuffer b=ByteBuffer.allocate(wide?24:12).order(order);
            if(wide)b.putLong(offset).putLong(info).putLong(addend);
            else b.putInt((int)offset).putInt((int)info).putInt((int)addend);
            return b.array();
        }
        public byte[] bytes() {
            data.set(1,names.toByteArray());headers.get(1)[5]=data.get(1).length;
            int eh=wide?64:52,sh=wide?64:40,pos=eh;
            for(int i=1;i<data.size();i++){headers.get(i)[4]=pos;pos+=data.get(i).length;}
            int table=(pos+7)&~7;
            ByteBuffer b=ByteBuffer.allocate(table+sh*data.size()).order(order);
            b.put(new byte[]{127,69,76,70,(byte)(wide?2:1),(byte)(order==ByteOrder.LITTLE_ENDIAN?1:2),1,0,0,0,0,0,0,0,0,0});
            b.putShort((short)1).putShort((short)(wide?62:3)).putInt(1);
            if(wide)b.putLong(0).putLong(0).putLong(table);else b.putInt(0).putInt(0).putInt(table);
            b.putInt(0).putShort((short)eh).putShort((short)0).putShort((short)0).putShort((short)sh).putShort((short)data.size()).putShort((short)1);
            for(int i=1;i<data.size();i++) { b.position((int)headers.get(i)[4]);b.put(data.get(i)); }
            for(int i=1;i<data.size();i++) {
                b.position(table+i*sh);long[] h=headers.get(i);
                b.putInt((int)h[0]).putInt((int)h[1]);
                if(wide){for(int j=2;j<6;j++)b.putLong(h[j]);b.putInt((int)h[6]).putInt((int)h[7]).putLong(h[8]).putLong(h[9]);}
                else for(int j=2;j<10;j++)b.putInt((int)h[j]);
            }
            return b.array();
        }
    }
    public static byte[] basic() {
        Image f=new Image(true,ByteOrder.LITTLE_ENDIAN);
        f.add(".text",1,new byte[64],0,0,0);
        int s=f.add(".strtab",3,new byte[]{0,'a','l','p','h','a',0},0,0,0);
        int t=f.add(".symtab",2,f.symbol(1,16),s,1,24);
        f.add(".rela.text",4,f.rela(8,(1L<<32)|2,-4),t,2,24);
        f.add(".android.rela",ANDROID_RELA,packed(2,0,2,3,8,(1L<<32)|2),t,2,1);
        return f.bytes();
    }

    /** The four supported file layouts: ELF32/64 crossed with little/big endian. */
    public enum Layout {
        ELF32_LSB(false, ByteOrder.LITTLE_ENDIAN),
        ELF32_MSB(false, ByteOrder.BIG_ENDIAN),
        ELF64_LSB(true, ByteOrder.LITTLE_ENDIAN),
        ELF64_MSB(true, ByteOrder.BIG_ENDIAN);

        public final boolean wide;
        public final ByteOrder order;

        Layout(boolean wide, ByteOrder order) {
            this.wide = wide;
            this.order = order;
        }
    }

    /** Section indices and encoded expectations for the mixed-group hand-written image. */
    public static final class Mixed {
        public byte[] bytes;
        public int strtab, dynstr, symtab, dynsym, ordinaryRela, ordinaryRel, packedRela, packedRel;
        public long info1, info2;
        /** Rows of {r_offset, r_info, r_addend} for the packed RELA section, in expansion order. */
        public long[][] relaExpectations;
        /** Rows of {r_offset, r_info} for the packed REL section, in expansion order. */
        public long[][] relExpectations;
    }

    /**
     * Builds an image with two symbol tables (each linked to its own, differently named string table),
     * ordinary REL/RELA controls and packed Android REL/RELA sections. The packed streams mix every
     * group flag combination, cross group boundaries, keep duplicate positions and use negative addends.
     */
    public static Mixed mixed(boolean wide, ByteOrder order) {
        Image f = new Image(wide, order);
        int symEnt = wide ? 24 : 16;
        long i1 = info(wide, 1, 2);
        long i2 = info(wide, 2, 3);

        f.add(".text", 1, new byte[64], 0, 0, 0);
        int strtab = f.add(".strtab", 3, strings("alpha", "beta"), 0, 0, 0);
        int dynstr = f.add(".dynstr", 3, strings("gamma", "delta"), 0, 0, 0);
        int symtab = f.add(".symtab", 2, f.symbols(new long[][] {{0, 0}, {1, 0x1000}, {7, 0x2000}}), strtab, 1, symEnt);
        int dynsym = f.add(".dynsym", 11, f.symbols(new long[][] {{0, 0}, {1, 0x3000}, {7, 0x4000}}), dynstr, 1, symEnt);

        ByteArrayOutputStream rela = new ByteArrayOutputStream();
        rela.writeBytes(f.rela(0x50, i1, -8));
        rela.writeBytes(f.rela(0x60, i2, 0));
        int ordinaryRela = f.add(".rela.text", 4, rela.toByteArray(), symtab, 2, wide ? 24 : 12);

        ByteArrayOutputStream rel = new ByteArrayOutputStream();
        rel.writeBytes(f.rel(0x70, i1));
        rel.writeBytes(f.rel(0x78, i2));
        int ordinaryRel = f.add(".rel.text", 9, rel.toByteArray(), symtab, 2, wide ? 16 : 8);

        // 6 records: grouped offset/info/addend, then per-record addends, then a group that resets
        // the running addend to zero. Positions 0x1010 and 0x1020 are each emitted twice.
        byte[] packedRelaBytes = packed(wide, 6, 0x1000,
                2, 0xF, 0x10, i1, -4,
                2, 0x8, 0x8, i2, -1, 0x4, i2, 5,
                2, 0x0, 0x4, i1, 0x0, i2);
        int packedRela = f.add(".android.rela", ANDROID_RELA, packedRelaBytes, dynsym, 0, 1);

        // 4 REL records: a grouped pair followed by a per-record pair, ending on a duplicate position.
        byte[] packedRelBytes = packed(wide, 4, 0x2000,
                2, 0x3, 0x20, i1,
                2, 0x0, 0x10, i2, 0x0, i1);
        int packedRel = f.add(".android.rel", ANDROID_REL, packedRelBytes, dynsym, 0, 1);

        Mixed m = new Mixed();
        m.bytes = f.bytes();
        m.strtab = strtab;
        m.dynstr = dynstr;
        m.symtab = symtab;
        m.dynsym = dynsym;
        m.ordinaryRela = ordinaryRela;
        m.ordinaryRel = ordinaryRel;
        m.packedRela = packedRela;
        m.packedRel = packedRel;
        m.info1 = i1;
        m.info2 = i2;
        m.relaExpectations = new long[][] {
                {0x1010, i1, -4},
                {0x1010, i1, -4},
                {0x1018, i2, -5},
                {0x101c, i2, 0},
                {0x1020, i1, 0},
                {0x1020, i2, 0},
        };
        m.relExpectations = new long[][] {
                {0x2020, i1},
                {0x2020, i1},
                {0x2030, i2},
                {0x2030, i1},
        };
        return m;
    }

    /**
     * An image containing one malformed packed section and, alongside it, one valid packed section and
     * one ordinary section, so failure isolation between sections can be checked.
     */
    public static byte[] withBadSection(boolean wide, ByteOrder order, int packedType, byte[] rawPacked) {
        Image f = new Image(wide, order);
        int symEnt = wide ? 24 : 16;
        int strtab = f.add(".strtab", 3, strings("alpha"), 0, 0, 0);
        int symtab = f.add(".symtab", 2, f.symbols(new long[][] {{0, 0}, {1, 0x1000}}), strtab, 1, symEnt);
        f.add(".rela.text", 4, f.rela(0x50, info(wide, 1, 2), -8), symtab, 2, wide ? 24 : 12);
        byte[] good = packed(wide, 1, 0x100, 1, 0x3, 0x8, info(wide, 1, 2));
        f.add(packedType == ANDROID_RELA ? ".good.android.rela" : ".good.android.rel",
                packedType, good, symtab, 0, 1);
        f.add(".bad.packed", packedType, rawPacked, symtab, 0, 1);
        return f.bytes();
    }

    /** Index of the malformed section produced by {@link #withBadSection}. */
    public static int badSectionIndex() {
        // null, shstrtab, text is absent: null, shstrtab, strtab, symtab, rela, good, bad
        return 6;
    }

    /** Index of the valid packed neighbor produced by {@link #withBadSection}. */
    public static int goodSectionIndex() {
        return 5;
    }

    /** A packed stream whose offset and r_info use the highest bits of the file width. */
    public static byte[] highBitStream(boolean wide) {
        if (wide) {
            return packed(true, 2, Long.MIN_VALUE, 2, 0x3, 0x10, 0xFFFFFFFF00000002L);
        }
        // 32-bit unsigned patterns must travel through their signed SLEB128 representation.
        // Initial offset -32 (0xFFFFFFE0) plus delta 0x10 stays inside the unsigned width.
        return packed(false, 2, -32, 2, 0x3, 0x10, (int) 0x80000102L);
    }

    /** Expected {r_offset, r_info} rows emitted by {@link #highBitStream(boolean)}. */
    public static long[][] highBitExpectations(boolean wide) {
        // The single group carries OFFSET_DELTA (0x10), so both records land at initial + 0x10.
        long initial = wide ? Long.MIN_VALUE : 0xFFFFFFE0L;
        long offset = initial + 0x10;
        long info = wide ? 0xFFFFFFFF00000002L : 0x80000102L;
        return new long[][] {{offset, info}, {offset, info}};
    }

    /** An image whose sole packed section carries highest-bit offset and r_info patterns. */
    public static byte[] highBitImage(boolean wide, ByteOrder order, int packedType) {
        Image f = new Image(wide, order);
        int symEnt = wide ? 24 : 16;
        int strtab = f.add(".strtab", 3, strings("alpha"), 0, 0, 0);
        int symtab = f.add(".symtab", 2, f.symbols(new long[][] {{0, 0}, {1, 0x1000}}), strtab, 1, symEnt);
        f.add(packedType == ANDROID_RELA ? ".android.rela" : ".android.rel",
                packedType, highBitStream(wide), symtab, 0, 1);
        return f.bytes();
    }

    /** An image with a zero-record packed stream, which still carries its initial offset. */
    public static byte[] emptyImage(boolean wide, ByteOrder order, int packedType) {
        Image f = new Image(wide, order);
        int symEnt = wide ? 24 : 16;
        int strtab = f.add(".strtab", 3, strings("alpha"), 0, 0, 0);
        int symtab = f.add(".symtab", 2, f.symbols(new long[][] {{0, 0}, {1, 0x1000}}), strtab, 1, symEnt);
        f.add(".android.empty", packedType, packed(wide, 0, 0x33), symtab, 0, 1);
        return f.bytes();
    }

    /** An image containing a single packed section linked to a symbol table. */
    public static byte[] singlePackedImage(boolean wide, ByteOrder order, int packedType, byte[] rawPacked) {
        Image f = new Image(wide, order);
        int symEnt = wide ? 24 : 16;
        int strtab = f.add(".strtab", 3, strings("alpha"), 0, 0, 0);
        int symtab = f.add(".symtab", 2, f.symbols(new long[][] {{0, 0}, {1, 0x1000}}), strtab, 1, symEnt);
        f.add(packedType == ANDROID_RELA ? ".android.rela" : ".android.rel",
                packedType, rawPacked, symtab, 0, 1);
        return f.bytes();
    }

    /** Index of the packed section produced by {@link #singlePackedImage}. */
    public static int singlePackedIndex() {
        // null, shstrtab, strtab, symtab, packed
        return 4;
    }




}
