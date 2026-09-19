// kotlin-test ships inside the kotlinc distribution at lib/kotlin-test.jar.
// On the JVM target its @Test annotation is a typealias to a JUnit annotation and
// JUnit is NOT shipped, so no test can be DECLARED; the shipped assertion API is
// the part of the first-party test library usable with the distribution alone.
import kotlin.test.assertEquals
import kotlin.test.assertTrue
fun testAdd() { assertEquals(2, 1 + 1); assertTrue(1 + 1 == 2) }
fun main() { testAdd(); println("X22 pass 1") }
