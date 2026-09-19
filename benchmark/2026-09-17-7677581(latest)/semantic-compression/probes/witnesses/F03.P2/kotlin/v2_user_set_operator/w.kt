class Grid { var last = 0; operator fun set(i: Int, value: Int) { last = i * 1000 + value } }
fun main() {
    val xs = Grid()
    xs[1] = 42
    println("user set -> ${xs.last}")
}
