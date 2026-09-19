class Main {
    static double probe(int i, int d) {
        // BEGIN PROBE F10.P2
        double sum = i + d;
        // END PROBE F10.P2
        return sum;
    }

    public static void main(String[] args) {
        System.out.println(probe(Integer.MAX_VALUE, 1));
    }
}
