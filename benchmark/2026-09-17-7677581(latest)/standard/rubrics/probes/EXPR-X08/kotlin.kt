fun inner(): Nothing = throw IllegalStateException("boom")
fun outer() { inner() }
fun main() { try { outer() } catch (e: RuntimeException) { println("X08 caught ${e.message}") } }
