/*
 * Decompiled with CFR 0.153-SNAPSHOT (c4145259).
 */
final class SeInverted {
    static int trace;

    SeInverted() {
    }

    static void scan(int n, int n2, int n3, int n4) {
        int n5;
        block3: {
            if (n2 < 0) {
                if (n < 0) {
                    n3 += n - n2;
                    n2 = n;
                    break block3;
                }
                n3 += -n2 * n4;
                n2 = 0;
            }
            n5 = n2;
            while (n > n2) {
                trace += n3 + n5;
                ++n2;
                n5 += 3;
                n3 += n4;
            }
        }
        n5 = n - n2;
        trace += n3 + n5;
    }
}
