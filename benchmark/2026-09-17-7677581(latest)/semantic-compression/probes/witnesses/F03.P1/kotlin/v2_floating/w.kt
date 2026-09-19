fun main() {
    var d = Double.MAX_VALUE
    d++
    println("Double.MAX_VALUE++ -> " + d + " (unchanged: no overflow edge exists)")
    var i = Int.MAX_VALUE
    i++
    println("Int.MAX_VALUE++ -> " + i + " (wraps modulo 2^32)")
}
