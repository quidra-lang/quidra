class Main {
    static byte probe() {
        // BEGIN PROBE F02.P2
        byte[] buf = new byte[16];
        buf[0] = 1;
        return buf[0];
        // END PROBE F02.P2
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
