class Upto(private val n: Int) : Iterable<Int> {
  override fun iterator() = object : Iterator<Int> { var i = 0
    override fun hasNext() = i < n
    override fun next() = i++ } }
fun main() { val sb = StringBuilder("X07"); for (v in Upto(3)) sb.append(" ").append(v); println(sb) }
