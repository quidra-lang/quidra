class Main {
    static double probe(Integer i, Double d) {
        // BEGIN PROBE F10.P2
        double sum = i + d;
        // END PROBE F10.P2
        return sum;
    }

    public static void main(String[] args) {
        System.out.println(probe(3, null));
    }
}
