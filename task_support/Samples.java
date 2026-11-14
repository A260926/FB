package task_support;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.Adler32;
/** Small inert, legal format examples. Contains no code that executes a DEX. */
public final class Samples {
  static void uleb(ByteBuffer b,int x) { do {int q=x&127;x>>>=7;b.put((byte)(q|(x==0?0:128)));}while(x!=0); }
  static void align(ByteBuffer b,int n){while(b.position()%n!=0)b.put((byte)0);}
  static int sum(byte[] b,int p,int n){Adler32 a=new Adler32();a.update(b,p,n);return (int)a.getValue();}
  public static byte[] image(boolean compact) throws Exception {
    ByteBuffer b=ByteBuffer.allocate(4096).order(ByteOrder.LITTLE_ENDIAN);
    int header=compact?0x88:0x70, strings=header, types=strings+20, proto=types+12;
    int methods=proto+12, classes=methods+8, dataBase=compact?0x200:0;
    int dataStart=compact?4:classes+32;
    b.position(dataBase+dataStart);
    String[] ss={"A.java","I","LA;","Ljava/lang/Object;","f"};
    int firstString=b.position()-dataBase;
    for(int i=0;i<ss.length;i++){
      b.putInt(strings+i*4,b.position()-dataBase);
      uleb(b,ss[i].length());b.put(ss[i].getBytes(StandardCharsets.US_ASCII)).put((byte)0);
    }
    int debug=b.position()-dataBase;
    b.put(new byte[]{42,0,14,0}); // line 42 at address zero; end sequence
    int debugTable=0,lookup=0;
    if(compact){
      debugTable=b.position()-dataBase;
      b.put(new byte[]{0,1,0}); // method zero present; delta zero from base
      align(b,4);lookup=b.position()-dataBase-debugTable;b.putInt(0);
    }
    align(b,4);int code=b.position()-dataBase;
    if(compact){b.putShort((short)0x1000).putShort((short)(2<<5));}
    else {b.putShort((short)1).putShort((short)0).putShort((short)0).putShort((short)0).putInt(debug).putInt(2);}
    b.putShort((short)0x7012).putShort((short)0x000f); // const/4 v0,7; return v0
    int classData=b.position()-dataBase;
    b.put(new byte[]{0,0,1,0,0,9});uleb(b,code);
    align(b,4);int map=b.position()-dataBase;
    int[][] rows={{0,1,0},{1,5,strings},{2,3,types},{3,1,proto},{5,1,methods},{6,1,classes},
      {0x2002,5,firstString},{0x2003,1,debug},{0x2001,1,code},{0x2000,1,classData},{0x1000,1,map}};
    b.putInt(rows.length);
    for(int[] r:rows)b.putShort((short)r[0]).putShort((short)0).putInt(r[1]).putInt(r[2]);
    int end=b.position(), mainSize=compact?dataBase:end;
    b.position(0);b.put((compact?"cdex001\0":"dex\n035\0").getBytes(StandardCharsets.ISO_8859_1));
    b.putInt(32,mainSize).putInt(36,header).putInt(40,0x12345678).putInt(52,map);
    b.putInt(56,5).putInt(60,strings).putInt(64,3).putInt(68,types);
    b.putInt(72,1).putInt(76,proto).putInt(88,1).putInt(92,methods).putInt(96,1).putInt(100,classes);
    b.putInt(104,end-(compact?dataBase:dataStart)).putInt(108,compact?dataBase:dataStart);
    b.putInt(types,1).putInt(types+4,2).putInt(types+8,3);
    b.putInt(proto,1).putInt(proto+4,0).putInt(proto+8,0);
    b.putShort(methods,(short)1).putShort(methods+2,(short)0).putInt(methods+4,4);
    b.putInt(classes,1).putInt(classes+4,1).putInt(classes+8,2).putInt(classes+12,0);
    b.putInt(classes+16,0).putInt(classes+20,0).putInt(classes+24,classData).putInt(classes+28,0);
    byte[] out=Arrays.copyOf(b.array(),end);
    if(compact){
      b=ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
      b.putInt(112,0).putInt(116,debugTable).putInt(120,lookup).putInt(124,debug);
      b.putInt(128,0).putInt(132,end-dataBase);
      byte[] h=Arrays.copyOf(out,header);ByteBuffer hb=ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN);
      hb.putInt(8,0).putInt(104,0).putInt(108,0);
      int c=sum(h,0,h.length);c=(c*31)^sum(out,header,mainSize-header);c=(c*31)^sum(out,dataBase,end-dataBase);b.putInt(8,c);
    }else{
      byte[] digest=MessageDigest.getInstance("SHA-1").digest(Arrays.copyOfRange(out,32,end));
      System.arraycopy(digest,0,out,12,20);ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN).putInt(8,sum(out,12,end-12));
    }
    return out;
  }
  public static void main(String[] args)throws Exception{
    java.nio.file.Path dir=java.nio.file.Paths.get(args[0]);java.nio.file.Files.createDirectories(dir);
    java.nio.file.Files.write(dir.resolve("ordinary.dex"),image(false));
    java.nio.file.Files.write(dir.resolve("minimal.cdex"),image(true));
    System.out.println("SAMPLES_WRITTEN ordinary.dex minimal.cdex");
  }
}
