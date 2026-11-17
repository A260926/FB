/*
 * Decompiled with CFR 0.153-SNAPSHOT (c4145259).
 */
final class SeContinue {
    static int trace;

    SeContinue() {
    }

    static void scan(int n, int n2, int n3) {
        int n4;
        if (n2 < 0) {
            n2 = n;
        } else {
            n4 = n2;
            while (n > n2) {
                ++n2;
                if (n3 == 0) continue;
                trace += n4;
            }
        }
        n4 = n - n2;
        trace += n3 + n4;
    }
}
