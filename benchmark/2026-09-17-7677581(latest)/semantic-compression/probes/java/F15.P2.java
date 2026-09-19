class Main {
    // BEGIN PROBE F15.P2
    static <T extends Comparable<T>> T maxOf(T a, T b) {
        return a.compareTo(b) > 0 ? a : b;
    }

    static int probe() {
        int m = maxOf(3, 5);
        return m;
    }
    // END PROBE F15.P2

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
