import java.io.File

fun main() {
    val r = File("data.txt").bufferedReader()
    try {
        r.use { throw RuntimeException("failure inside the body") }
    } catch (e: RuntimeException) {
        println("body threw: " + e.message)
    }
    try {
        r.read()
        println("handle still open")
    } catch (e: java.io.IOException) {
        println("handle released: " + e.message)
    }
    try {
        File("missing.txt").bufferedReader().use { it.readText() }
    } catch (e: java.io.FileNotFoundException) {
        println("open failure propagates: " + e.javaClass.name)
    }
}
