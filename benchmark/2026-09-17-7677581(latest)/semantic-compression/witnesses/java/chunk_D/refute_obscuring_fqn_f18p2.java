class Main {
    static Object util = new Object();
    static int callUtil() {
        int result = util.Util.pubAdd(2, 3);
        return result;
    }
    public static void main(String[] a) { System.out.println(callUtil()); }
}
