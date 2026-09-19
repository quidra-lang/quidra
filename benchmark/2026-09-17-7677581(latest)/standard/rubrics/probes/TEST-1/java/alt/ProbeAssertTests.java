// Supplementary (not the frozen recipe's route): TEST-1 expressed with the JDK's
// built-in `assert` statement, run with `java -ea`. Recorded to test whether an
// admissible first-party assertion construct alone can satisfy V2/V3 for Java.
public class ProbeAssertTests {
    static int add(int a, int b) { return a + b; }

    static void passOne() { assert add(1, 1) == 2 : "pass_one"; }
    static void passTwo() { assert add(2, 3) == 5 : "pass_two"; }
    static void failOne() { assert add(1, 1) == 3 : "fail_one"; }

    public static void main(String[] args) {
        passOne();
        passTwo();
        failOne();
        System.out.println("all assertions held");
    }
}
