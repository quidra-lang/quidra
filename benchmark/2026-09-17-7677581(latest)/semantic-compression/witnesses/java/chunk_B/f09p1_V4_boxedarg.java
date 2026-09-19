class Main {
    static boolean probe() {
        String s1 = new StringBuilder("ab").append("c").toString();
        int s2 = 3;
        // BEGIN PROBE F09.P1
        boolean eq = s1.equals(s2);
        // END PROBE F09.P1
        return eq;
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
