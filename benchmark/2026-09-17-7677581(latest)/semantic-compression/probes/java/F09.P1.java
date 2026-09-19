class Main {
    static boolean probe() {
        String s1 = new StringBuilder("ab").append("c").toString();
        String s2 = new StringBuilder("ab").append("c").toString();
        // BEGIN PROBE F09.P1
        boolean eq = s1.equals(s2);
        // END PROBE F09.P1
        return eq;
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
