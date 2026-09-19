import java.io.File
fun main() { File("x18.txt").writeText("hello")
  val d = File("x18.txt").readText(); println("X18 $d ${File("x18.txt").exists()}") }
