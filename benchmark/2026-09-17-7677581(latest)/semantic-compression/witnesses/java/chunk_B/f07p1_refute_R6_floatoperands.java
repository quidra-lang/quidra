class Main {
    static int probe(double a, double b, double c) {
        // BEGIN PROBE F07.P1
        int r = a * b + c;
        // END PROBE F07.P1
        return r;
    }

    public static void main(String[] args) {
        System.out.println(probe(2.0, 3.0, 4.0));
    }
}
