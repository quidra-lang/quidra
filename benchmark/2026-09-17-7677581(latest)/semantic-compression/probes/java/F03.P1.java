class Main {
    static int probe() {
        int x = 41;
        // BEGIN PROBE F03.P1
        x++;
        // END PROBE F03.P1
        return x;
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
