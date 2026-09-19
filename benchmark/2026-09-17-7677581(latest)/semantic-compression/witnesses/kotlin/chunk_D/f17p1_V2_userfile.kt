class UserReader {
    fun readText(): String = "0123456789"
}
class File(val name: String) {
    fun bufferedReader(): UserReader = UserReader()
}
fun <T, R> T.use(block: (T) -> R): R = block(this)

// BEGIN PROBE F17.P1
fun readAll(): Int {
    val text = File("data.txt").bufferedReader().use { it.readText() }
    return text.toByteArray().size
}
// END PROBE F17.P1

fun main() { println(readAll()) }
