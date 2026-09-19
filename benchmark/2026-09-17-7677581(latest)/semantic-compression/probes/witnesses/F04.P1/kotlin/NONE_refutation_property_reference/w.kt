fun probe(): Int {
    var slot = 4
    var port by ::slot
    port = 9
    return slot
}
fun main() { println(probe()) }
