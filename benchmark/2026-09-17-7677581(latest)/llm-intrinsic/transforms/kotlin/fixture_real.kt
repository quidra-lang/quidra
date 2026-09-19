fun main() {
    var state = 7L
    var sum = 0L
    var max = 0L
    var evens = 0
    var joined = ""
    for (i in 0..49) {
        state = (state * 48271) % 2147483647
        val term = state % 1000
        sum = sum + term
        if (term > max) {
            max = term
        }
        if (term % 2 == 0L) {
            evens = evens + 1
        }
        if (i < 5) {
            if (i == 0) {
                joined = joined + term
            } else {
                joined = joined + "-" + term
            }
        }
    }
    println("SUM " + sum)
    println("MAX " + max)
    println("EVENS " + evens)
    println("JOINED " + joined)
}
