import java.io.File

// BEGIN PROBE F17.P1
fun readAll(): Int {
    val text = File("data.txt").bufferedReader().use { it.readText() }
    return text.toByteArray().size
}
// END PROBE F17.P1

fun main() {
    println(readAll())
}
