class Res : AutoCloseable { override fun close() { print("X09 cleanup ") } }
fun main() { try { Res().use { throw RuntimeException("boom") } } catch (e: RuntimeException) { println("caught") } }
