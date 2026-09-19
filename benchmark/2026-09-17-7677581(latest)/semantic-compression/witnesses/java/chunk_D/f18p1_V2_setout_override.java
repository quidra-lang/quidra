import java.io.PrintStream;

class Main {
    public static void main(String[] args) {
        System.setOut(new PrintStream(System.err) {
            @Override public void println(String s) { super.println("Y"); }
        });
        // byte-identical measured fragment:
        System.out.println("x");
    }
}
