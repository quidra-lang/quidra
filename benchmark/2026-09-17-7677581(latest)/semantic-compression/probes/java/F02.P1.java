class Main {
    static int probe(boolean cond) {
        // BEGIN PROBE F02.P1
        int v;
        if (cond) v = 5; else v = 9;
        return v;
        // END PROBE F02.P1
    }

    public static void main(String[] args) {
        System.out.println(probe(true));
    }
}
