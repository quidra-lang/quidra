class Main {
    static int probe(Long big) {
        // BEGIN PROBE F10.P1
        int small = (int) big;
        return small;
        // END PROBE F10.P1
    }

    public static void main(String[] args) {
        System.out.println(probe(2147483648L));
    }
}
