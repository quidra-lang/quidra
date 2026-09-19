class Main {
    static String probe(int a, int b) {
        // BEGIN PROBE F07.P2
        int q = a / b;
        int m = a % b;
        double d = (double) a / b;
        // END PROBE F07.P2
        return q + " " + m + " " + d;
    }

    public static void main(String[] args) {
        System.out.println(probe(Integer.MIN_VALUE, -1));
    }
}
