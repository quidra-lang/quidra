var released = 0

class Handle(val id: Int) : AutoCloseable {
    override fun close() { released++ }
}

fun make() { Handle(1) }

fun main() {
    make()
    System.gc()
    Thread.sleep(200)
    System.gc()
    Thread.sleep(200)
    println("released after the value became unreachable = " + released)
}
