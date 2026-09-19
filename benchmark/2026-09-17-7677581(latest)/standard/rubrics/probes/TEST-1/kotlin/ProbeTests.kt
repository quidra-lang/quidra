// TEST-1 probe, Kotlin. Exactly 3 tests using kotlin.test (kotlin-test.jar ships in
// the kotlinc distribution): two assert a true condition, one asserts a false condition.
import kotlin.test.Test
import kotlin.test.assertTrue

class ProbeTests {
    @Test
    fun passOne() { assertTrue(add(1, 1) == 2) }

    @Test
    fun passTwo() { assertTrue(add(2, 3) == 5) }

    @Test
    fun failOne() { assertTrue(add(1, 1) == 3) }
}
