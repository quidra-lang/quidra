class Main {
    static int probe(Integer a, Integer b, Integer c) {
        // BEGIN PROBE F07.P1
        int r = a * b + c;
        // END PROBE F07.P1
        return r;
    }

    public static void main(String[] args) {
        System.out.println(probe(null, 3, 4));
    }
}
