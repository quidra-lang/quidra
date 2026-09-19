class Main {
    static int overflowAtMax() {
        // BEGIN PROBE F08.P1
        int m = Integer.MAX_VALUE;
        int o = m + 1;
        return o;
        // END PROBE F08.P1
    }

    public static void main(String[] args) {
        System.out.println(overflowAtMax());
    }
}
