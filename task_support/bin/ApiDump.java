import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.*;
import org.benf.cfr.reader.api.*;

// Convenience entry to the real library API. Never loads the input class.
public final class ApiDump {
    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("input.class output.java");
        final List<String> texts = new ArrayList<String>();
        OutputSinkFactory factory = new OutputSinkFactory() {
            public List<SinkClass> getSupportedSinks(SinkType type, Collection<SinkClass> available) {
                return Collections.singletonList(type == SinkType.JAVA ? SinkClass.DECOMPILED : SinkClass.STRING);
            }
            public <T> Sink<T> getSink(final SinkType type, SinkClass cls) {
                return new Sink<T>() {
                    public void write(T value) {
                        if (type == SinkType.JAVA) texts.add(((SinkReturns.Decompiled)value).getJava());
                        if (type == SinkType.EXCEPTION) throw new IllegalStateException(String.valueOf(value));
                    }
                };
            }
        };
        new CfrDriver.Builder().withOutputSink(factory).build().analyse(Collections.singletonList(args[0]));
        if (texts.size()!=1 || texts.get(0).trim().isEmpty()) throw new IllegalStateException("expected one class");
        Files.write(Paths.get(args[1]), texts.get(0).getBytes(StandardCharsets.UTF_8));
    }
}
