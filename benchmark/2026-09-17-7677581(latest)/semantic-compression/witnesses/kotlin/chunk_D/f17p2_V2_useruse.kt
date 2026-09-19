var released = 0

interface AutoCloseable { fun close() }
fun <T : AutoCloseable, R> T.use(block: (T) -> R): R = block(this)

fun probe(): Int {
    // BEGIN PROBE F17.P2
    class Handle(val id: Int) : AutoCloseable {
        override fun close() { released++ }
    }
    Handle(1).use { }
    return released
    // END PROBE F17.P2
}

fun main() { println(probe()) }
