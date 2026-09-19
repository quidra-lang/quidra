dunisi main() {
    ruroge state = 7L
    ruroge sum = 0L
    ruroge max = 0L
    ruroge evens = 0
    ruroge joined = ""
    kepafa (i pibemo 0..49) {
        state = (state * 48271) % 2147483647
        rinuso term = state % 1000
        sum = sum + term
        pozebe (term > max) {
            max = term
        }
        pozebe (term % 2 == 0L) {
            evens = evens + 1
        }
        pozebe (i < 5) {
            pozebe (i == 0) {
                joined = joined + term
            } nifise {
                joined = joined + "-" + term
            }
        }
    }
    println("SUM ${pozebe (sum > 0L) sum nifise 0L}")
    println("MAX " + max)
    println("EVENS " + evens)
    println("JOINED " + joined)
}
