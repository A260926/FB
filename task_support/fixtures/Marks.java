import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Type-use annotations used by the local variable type annotation fixtures.
 * The sources are only read and compiled by the JDK tools; they are never loaded or executed.
 */
@Target(ElementType.TYPE_USE)
@Retention(RetentionPolicy.RUNTIME)
@interface Mark {
  String value();
}

@Target(ElementType.TYPE_USE)
@Retention(RetentionPolicy.CLASS)
@interface Hidden {
  String value();
}
