class Main {
    static int handled(String s) {
        // BEGIN PROBE F13.P2
        int n;
        try {
            n = Integer.parseInt(s);
        } catch (NumberFormatException e) {
            n = 0;
        }
        return n;
        // END PROBE F13.P2
    }

    public static void main(String[] args) {
        System.out.println(handled("21"));
    }
}
