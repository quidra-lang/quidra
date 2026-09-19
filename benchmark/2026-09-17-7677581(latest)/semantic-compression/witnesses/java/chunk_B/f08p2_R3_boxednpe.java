class Main {
    static int guarded(Integer a, Integer b) {
        // BEGIN PROBE F08.P2
        int q = b == 0 ? 0 : a / b;
        return q;
        // END PROBE F08.P2
    }

    public static void main(String[] args) {
        System.out.println(guarded(7, null));
    }
}
