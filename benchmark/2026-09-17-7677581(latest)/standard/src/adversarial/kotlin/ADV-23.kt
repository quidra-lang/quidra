import java.io.File

fun main() {
    println("ADV-START")
    System.out.flush()
    val s = File("inputs/ADV-23.bin").readText()
    println("OBS=CP:" + s.length)
    System.out.flush()
    println("ADV-END")
    System.out.flush()
}
