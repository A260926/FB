import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Marker annotation retained at {@link RetentionPolicy#CLASS}, mirroring a
 * {@code RuntimeInvisibleTypeAnnotations} entry.  It is not visible through reflection at run
 * time but must survive decompilation and recompilation with the same visibility and placement.
 */
@Target(ElementType.TYPE_USE)
@Retention(RetentionPolicy.CLASS)
public @interface Trusted {
    String value() default "";
}
