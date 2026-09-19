class Main {
    static class util {
        static int pubAdd(int a, int b) { return a + b + secret(); }
        private static int secret() { return 1; }
    }

    public static void main(String[] args) {
        // the boundary is NOT enforced: Main reaches the private member of the nested class
        System.out.println(util.pubAdd(2, 3) + " and secret() is reachable from outside util: " + util.secret());
    }
}
