class Ctr(val v: Int) { operator fun inc() = Ctr(v + 100) }
fun main() {
    var x = Ctr(1)
    x++
    println("user inc -> ${x.v}")
}
