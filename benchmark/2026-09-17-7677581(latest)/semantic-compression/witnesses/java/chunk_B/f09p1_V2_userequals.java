class Main {
    static class Text {
        @Override public boolean equals(Object other) {
            System.out.println("user equals");
            throw new IllegalStateException("user equality threw");
        }
        @Override public int hashCode() { return 0; }
    }

    static boolean probe() {
        Text s1 = new Text();
        Text s2 = new Text();
        // BEGIN PROBE F09.P1
        boolean eq = s1.equals(s2);
        // END PROBE F09.P1
        return eq;
    }

    public static void main(String[] args) {
        System.out.println(probe());
    }
}
