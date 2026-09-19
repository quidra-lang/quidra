import kotlin.reflect.KMutableProperty0
fun put(cell: KMutableProperty0<Int>) { cell.set(12) }
fun probe(): Int {
    var cell = 3
    put(::cell)
    return cell
}
fun main() { println(probe()) }
