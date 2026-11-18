/**
 * Switch expression fixtures compiled with both debug information and
 * '-g:none'. They cover dense (tableswitch) and sparse (lookupswitch)
 * selectors, shared labels, block/yield arms, throwing arms, arms with an
 * embedded 'if', nested switch expressions, the int/long/String result
 * types (including a null arm) and the return / assignment / arithmetic /
 * method-argument consumption positions.
 */
public class SwitchExprSamples {

    // Dense tableswitch, direct values, return consumer.
    public static int denseReturn(int selector) {
        return switch (selector) {
            case 0 -> 10;
            case 1 -> 11;
            case 2 -> 12;
            case 3 -> 13;
            case 4 -> 14;
            default -> 99;
        };
    }

    // Sparse lookupswitch, shared labels, assignment consumer.
    public static int sparseAssign(int selector) {
        int result = switch (selector) {
            case 10, 100 -> 1;
            case 1000 -> 2;
            case -500 -> 3;
            default -> 0;
        };
        return result;
    }

    // Long, two-slot result merged from several arms.
    public static long longResult(int selector) {
        return switch (selector) {
            case 1 -> 100_000_000_000L;
            case 2 -> 2L;
            default -> 0L;
        };
    }

    // Reference result type with a null arm.
    public static String stringResult(int selector) {
        return switch (selector) {
            case 1 -> "one";
            case 2 -> null;
            default -> "other";
        };
    }

    // Block arm with local statements ending in yield, plus a throwing arm.
    public static int blockYieldAndThrow(int selector) {
        return switch (selector) {
            case 1 -> {
                int doubled = selector * 2;
                yield doubled;
            }
            case 2 -> throw new IllegalStateException("two");
            default -> 3;
        };
    }

    // Two normal exits of an embedded 'if' both belong to the same switch.
    public static int ifInsideArm(int selector, int other) {
        return switch (selector) {
            case 1 -> {
                if (other > 0) yield 10;
                yield 20;
            }
            default -> 0;
        };
    }

    // Nested switch expressions: inner yields must not be attributed to the outer.
    public static int nestedSwitch(int first, int second) {
        return switch (first) {
            case 1 -> switch (second) {
                case 1 -> 111;
                case 2 -> 222;
                default -> 333;
            };
            default -> 9;
        };
    }

    // Arithmetic sub-expression: the left operand is already stacked when
    // the switch is evaluated.
    public static int arithmeticConsumer(int left, int selector) {
        return left + switch (selector) {
            case 1 -> 5;
            default -> 6;
        };
    }

    // Method argument consumer with an earlier argument already stacked.
    public static int argumentConsumer(int fixed, int selector) {
        return Math.addExact(fixed, switch (selector) {
            case 1 -> 7;
            default -> 8;
        });
    }

    // Instance receiver kept on the stack while the switch argument is built.
    public int receiverConsumer(int selector) {
        return consume(switch (selector) {
            case 1 -> 1;
            default -> 2;
        });
    }

    // Boolean result: int constants must render as true/false.
    public static boolean booleanResult(int selector) {
        return switch (selector) {
            case 1 -> true;
            case 2 -> false;
            default -> true;
        };
    }

    // char and double (category-2 floating) results.
    public static char charResult(int selector) {
        return switch (selector) {
            case 1 -> 'a';
            case 2 -> 'b';
            default -> 'z';
        };
    }

    public static double doubleResult(int selector) {
        return switch (selector) {
            case 1 -> 1.5;
            default -> 2.5;
        };
    }

    // if/else inside a block arm, with a local variable merged into a yield.
    public static int ifElseInsideArm(int selector, int other) {
        return switch (selector) {
            case 1 -> {
                int result;
                if (other == 0) result = 10;
                else result = 20;
                yield result;
            }
            default -> 0;
        };
    }

    private int consume(int value) {
        return value;
    }
}
