class Main {
    static int callUtil() {
        // BEGIN PROBE F18.P2
        int result = util.Util.pubAdd(2, 3);
        return result;
        // END PROBE F18.P2
    }

    public static void main(String[] args) {
        System.out.println(callUtil());
    }
}
