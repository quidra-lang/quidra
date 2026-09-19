private var backing = 0
var released: Int
    get() = backing
    set(v) { backing = v + 40 }

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
