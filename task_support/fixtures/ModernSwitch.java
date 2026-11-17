public class ModernSwitch {
    public static int choose(int value) {
        return 10 + switch(value) {
            case 0, 1 -> 11;
            case 100 -> { int adjusted=value+1; yield adjusted; }
            default -> 17;
        };
    }
}
