package util;

// BEGIN PROBE F18.P2
public class Util {
    public static int pubAdd(int a, int b) {
        return a + b + secret();
    }

    private static int secret() {
        return 1;
    }
}
// END PROBE F18.P2
