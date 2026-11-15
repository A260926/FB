import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Marker annotation retained at {@link RetentionPolicy#RUNTIME}, mirroring a
 * {@code RuntimeVisibleTypeAnnotations} entry.  It carries no executable logic; it exists only
 * to label a type usage for offline integrity auditing.
 */
@Target(ElementType.TYPE_USE)
@Retention(RetentionPolicy.RUNTIME)
public @interface Sensitive {
    String value() default "";
}
