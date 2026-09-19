package probe;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.assertEquals;

public class ProjectTests {
    @Test
    void scaleWorks() { assertEquals(6, ModA.scale(2)); }

    @Test
    void scaleAndOffsetWorks() { assertEquals(7, ModB.scaleAndOffset(2)); }
}
