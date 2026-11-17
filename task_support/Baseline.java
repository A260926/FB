package net.fornwall.jelf;
import java.util.Arrays;
public final class Baseline {
 public static void main(String[] args) {
  byte[] raw=Fixture.basic(),copy=raw.clone();ElfFile f=ElfFile.from(raw);
  ElfRelocationAddend r=((ElfRelocationAddendSection)f.getSection(5)).relocations[0];
  if(r.r_offset!=8||r.r_addend!=-4||!"alpha".equals(r.getSymbol().getName()))throw new AssertionError("ordinary RELA");
  ElfSection packed=f.getSection(6);
  if(packed.getData().length==0||!Arrays.equals(raw,copy))throw new AssertionError("raw view");
  System.out.println("BASELINE_RELA offset="+r.r_offset+" addend="+r.r_addend+" symbol="+r.getSymbol().getName());
  System.out.println("ANDROID_SECTION class="+packed.getClass().getSimpleName()+" bytes="+packed.getData().length);
  System.out.println("BASELINE_OK");
 }
}
