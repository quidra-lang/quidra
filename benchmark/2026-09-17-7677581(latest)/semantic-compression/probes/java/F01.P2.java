class Main {
    // BEGIN PROBE F01.P2
    static long counter = 0;
    static final long LIMIT = 100;
    // END PROBE F01.P2

    public static void main(String[] args) {
        counter = counter + LIMIT;
        System.out.println(counter);
    }
}
