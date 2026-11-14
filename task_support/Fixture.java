import java.lang.annotation.*;
import java.util.List;
@Retention(RetentionPolicy.RUNTIME) @Target(ElementType.TYPE_USE)
@interface TUse { String value(); }
public class Fixture {
    @Deprecated
    public List<@TUse("argument") String @TUse("elementArray") []> @TUse("outerArray") [] values;
    public record Plain(int value) { }
}
