// TEST-1 probe, Java. Exactly 3 tests using JUnit 5 (org.junit.jupiter), the test
// framework admissible for Java under the three-clause tool admissibility test:
// two assert a true condition, one asserts a false condition.
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class ProbeTests {
    @Test
    void passOne() { assertTrue(Probe.add(1, 1) == 2); }

    @Test
    void passTwo() { assertTrue(Probe.add(2, 3) == 5); }

    @Test
    void failOne() { assertTrue(Probe.add(1, 1) == 3); }
}
