var backing = 41
var log = 0

var x: Int
    get() { log++; return backing }
    set(value) { log += 100; backing = value }

fun probe(): Int {
    // BEGIN PROBE F03.P1
    x++
    // END PROBE F03.P1
    return x
}

fun main() {
    println("result=" + probe() + " accessor side effects on a variable NOT named at the site: log=" + log)
}
