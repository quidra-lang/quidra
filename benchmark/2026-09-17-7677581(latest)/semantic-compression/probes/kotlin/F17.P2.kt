var released = 0

fun probe(): Int {
    // BEGIN PROBE F17.P2
    class Handle(val id: Int) : AutoCloseable {
        override fun close() { released++ }
    }
    Handle(1).use { h -> }
    return released
    // END PROBE F17.P2
}

fun main() {
    println(probe())
}
