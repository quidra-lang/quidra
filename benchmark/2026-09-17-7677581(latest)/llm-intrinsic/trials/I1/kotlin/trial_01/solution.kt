fun main() {
    var state = 7L
    var total = 0L
    var largest = 0L
    var evens = 0
    var joined = ""
    for (i in 0..49) {
        state = (state * 48271) % 2147483647
        val term = state % 1000
        total = total + term
        if (term > largest) {
            largest = term
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
    println("SUM " + total)
    println("MAX " + largest)
    println("EVENS " + evens)
    println("JOINED " + joined)
}
