package util

class Int(val v: kotlin.Int) {
    operator fun plus(o: Int) = Int(v + o.v + 100)
    override fun toString() = "UserInt(" + v + ")"
}
