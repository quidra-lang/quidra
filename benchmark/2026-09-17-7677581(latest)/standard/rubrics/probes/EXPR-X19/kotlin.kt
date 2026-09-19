import kotlin.concurrent.thread
fun main() { var box = 0; val t = thread { box = 42 }; t.join(); println("X19 $box") }
