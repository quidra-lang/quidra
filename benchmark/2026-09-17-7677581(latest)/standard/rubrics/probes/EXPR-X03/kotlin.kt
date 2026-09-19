class Box<T>(private val v: T) { fun get(): T = v }
fun main() { println("X03 ${Box(5).get()} ${Box("hi").get()}") }
