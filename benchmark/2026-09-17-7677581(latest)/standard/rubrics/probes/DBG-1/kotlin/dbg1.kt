// DBG-1
class Item(val count: Int, val name: String)

const val ITERATIONS: Long = 300000000L
const val MODULUS: Long = 1000000007L

fun accumulate(items: ArrayList<Item>, iterations: Long): Long {
    var total: Long = 0
    var i: Long = 0
    while (i < iterations) {
        for (item in items) {
            total = (total * 31 + item.count + item.name.length) % MODULUS
        }
        i++
    }
    return total
}

fun main() {
    val items = ArrayList<Item>()
    items.add(Item(7, "alpha"))
    items.add(Item(11, "bravo"))
    items.add(Item(13, "charlie"))
    val checksum = accumulate(items, ITERATIONS)
    println("checksum=" + checksum)
}
