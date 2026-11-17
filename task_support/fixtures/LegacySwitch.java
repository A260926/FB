public class LegacySwitch {
    public static void mark(int value) {}
    public static void choose(int value) {
        switch(value) {
            case 1: mark(2);
            case 2: mark(3); break;
            default: mark(7);
        }
    }
}
