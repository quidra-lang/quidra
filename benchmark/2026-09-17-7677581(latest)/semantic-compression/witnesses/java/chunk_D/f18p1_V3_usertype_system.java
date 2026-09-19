class System {
    static Out out = new Out();
}

class Out {
    void println(String s) { java.lang.System.out.println("user System.out.println, nothing named " + s + " reached the console facility"); }
}

class Main {
    public static void main(String[] args) {
        // BEGIN PROBE F18.P1
        System.out.println("x");
        // END PROBE F18.P1
    }
}
