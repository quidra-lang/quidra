class Main {
    static Object callUtil() {
        // BEGIN PROBE F18.P2
        int result = util.Util.pubAdd(2, 3);
        return result;
        // END PROBE F18.P2
    }

    public static void main(String[] args) {
        Object r = callUtil();
        java.lang.System.out.println(r.getClass().getName() + " " + r);
    }
}
