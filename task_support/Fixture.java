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
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        out.writeBytes(new byte[]{65,80,83,50});
        for(long v:words) {
            boolean more;
            do { int b=(int)(v&127); v>>=7;
                more=!((v==0&&(b&64)==0)||(v==-1&&(b&64)!=0));
                out.write(more ? b|128 : b);
            } while(more);
        }
        return out.toByteArray();
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
}
