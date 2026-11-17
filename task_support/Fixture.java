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
    public static byte[] concat(byte[]... parts) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        for(byte[] part:parts) out.writeBytes(part);
        return out.toByteArray();
    }
    /** Encodes one signed LEB128 integer. */
    public static byte[] sleb(long value) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        boolean more;
        do { int b=(int)(value&127); value>>=7;
            more=!((value==0&&(b&64)==0)||(value==-1&&(b&64)!=0));
            out.write(more ? b|128 : b);
        } while(more);
        return out.toByteArray();
    }
    /** Builds an APS2 stream from raw (possibly malformed) SLEB128 chunks. */
    public static byte[] aps2(byte[]... chunks) {
        byte[][] all=new byte[chunks.length+1][];
        all[0]=new byte[]{65,80,83,50};
        System.arraycopy(chunks,0,all,1,chunks.length);
        return concat(all);
    }
    public static byte[] packed(long... words) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        out.writeBytes(new byte[]{65,80,83,50});
        for(long v:words) out.writeBytes(sleb(v));
        return out.toByteArray();
    }
    /** A null-terminated string table body; index 0 is the empty string. */
    public static byte[] strings(String... values) {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        out.write(0);
        for(String value:values) {
            out.writeBytes(value.getBytes(StandardCharsets.US_ASCII));
            out.write(0);
        }
        return out.toByteArray();
    }
    public static final class Image {
        public final boolean wide; public final ByteOrder order;
        private final boolean bigEndian;
        private final List<byte[]> data=new ArrayList<>();
        private final List<long[]> headers=new ArrayList<>();
        private final ByteArrayOutputStream names=new ByteArrayOutputStream();
        public Image(boolean wide, ByteOrder order) {
            this.wide=wide;this.order=order;
            this.bigEndian=order==ByteOrder.BIG_ENDIAN;
            names.write(0);
            add("",0,new byte[0],0,0,0);
            add(".shstrtab",3,new byte[0],0,0,0);
        }
        public int add(String name,int type,byte[] bytes,int link,int info,int entrySize) {
            int n=names.size(); names.writeBytes(name.getBytes(StandardCharsets.US_ASCII));names.write(0);
            headers.add(new long[]{n,type,0,0,0,bytes.length,link,info,1,entrySize});
            data.add(bytes.clone());return data.size()-1;
        }
        public int entrySize() { return wide?24:16; }
        /** One symbol-table entry (a pair of these forms one archive slot in {@link #symbol}). */
        public byte[] entry(int name,long value) { return entry(name,value,0x12); }
        public byte[] entry(int name,long value,int info) {
            ByteBuffer b=ByteBuffer.allocate(wide?24:16).order(order);
            b.putInt(name);
            if(wide)b.put((byte)info).put((byte)0).putShort((short)2).putLong(value).putLong(4);
            else b.putInt((int)value).putInt(4).put((byte)info).put((byte)0).putShort((short)2);
            return b.array();
        }
        /** Two-entry symbol blob: the implicit null entry followed by the given named symbol. */
        public byte[] symbol(int name,long value) { return symbol(name,value,0x12); }
        public byte[] symbol(int name,long value,int info) {
            return concat(entry(0,0),entry(name,value,info));
        }
        /** A symbol-table body with a leading null entry followed by the given entries. */
        public byte[] symbols(byte[]... entries) {
            byte[][] all=new byte[entries.length+1][];
            all[0]=entry(0,0);
            System.arraycopy(entries,0,all,1,entries.length);
            return concat(all);
        }
        public byte[] rela(long offset,long info,long addend) {
            ByteBuffer b=ByteBuffer.allocate(wide?24:12).order(order);
            if(wide)b.putLong(offset).putLong(info).putLong(addend);
            else b.putInt((int)offset).putInt((int)info).putInt((int)addend);
            return b.array();
        }
        public byte[] rel(long offset,long info) {
            ByteBuffer b=ByteBuffer.allocate(wide?16:8).order(order);
            if(wide)b.putLong(offset).putLong(info); else b.putInt((int)offset).putInt((int)info);
            return b.array();
        }
        /** An r_info value for this image's word width. */
        public long info(int symbolIndex,long type) {
            return wide ? ((long)symbolIndex<<32)|(type&0xffffffffL) : ((long)symbolIndex<<8)|(type&0xff);
        }
        public byte[] bytes() {
            data.set(1,names.toByteArray());headers.get(1)[5]=data.get(1).length;
            int eh=wide?64:52,sh=wide?64:40,pos=eh;
            for(int i=1;i<data.size();i++){headers.get(i)[4]=pos;pos+=data.get(i).length;}
            int table=(pos+7)&~7;
            ByteBuffer b=ByteBuffer.allocate(table+sh*data.size()).order(order);
            b.put(new byte[]{127,69,76,70,(byte)(wide?2:1),(byte)(bigEndian?2:1),1,0,0,0,0,0,0,0,0,0});
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
}
