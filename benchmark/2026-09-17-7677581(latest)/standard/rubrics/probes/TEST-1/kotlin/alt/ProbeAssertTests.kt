// Supplementary: TEST-1 expressed with kotlin.test assertion functions only
// (kotlin-test.jar ships in the kotlinc distribution), driven from main(),
// because no JUnit platform runner is installed on this host.
import kotlin.test.assertTrue

fun add(a: Int, b: Int): Int = a + b

fun passOne() { assertTrue(add(1, 1) == 2, "pass_one") }
fun passTwo() { assertTrue(add(2, 3) == 5, "pass_two") }
fun failOne() { assertTrue(add(1, 1) == 3, "fail_one") }

fun main() {
    passOne()
    passTwo()
    failOne()
    println("all assertions held")
}
