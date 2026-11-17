package task_support;
import java.nio.file.*;
import org.jd.core.v1.ClassFileToJavaSourceDecompiler;
import org.jd.core.v1.api.loader.*;
import org.jd.core.v1.printer.PlainTextPrinter;
public final class Analyze {
    public static void main(String[] args) throws Exception {
        if(args.length!=2) throw new IllegalArgumentException("class directory and internal name required");
        final Path root=Paths.get(args[0]);
        Loader loader=new Loader() {
            public boolean canLoad(String name) {return Files.isRegularFile(root.resolve(name+".class"));}
            public byte[] load(String name) throws LoaderException {
                try {return Files.readAllBytes(root.resolve(name+".class"));}
                catch(java.io.IOException e){throw new LoaderException(e.toString());}
            }
        };
        PlainTextPrinter printer=new PlainTextPrinter();
        new ClassFileToJavaSourceDecompiler().decompile(loader,printer,args[1]);
        System.out.print(printer.toString());
    }
}
