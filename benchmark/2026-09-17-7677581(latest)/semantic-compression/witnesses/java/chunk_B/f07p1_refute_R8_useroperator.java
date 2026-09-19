class Main {
    static class M {}

    static int probe(M a, M b, M c) {
        // BEGIN PROBE F07.P1
        int r = a * b + c;
        // END PROBE F07.P1
        return r;
    }

    public static void main(String[] args) {
        System.out.println(probe(new M(), new M(), new M()));
    }
}
