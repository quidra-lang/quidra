fun probe(): Any {
    // BEGIN PROBE F18.P2
    val result = util.pubAdd(2, 3)
    return result
    // END PROBE F18.P2
}
fun main() { val r = probe(); println(r.javaClass.toString() + " " + r) }
