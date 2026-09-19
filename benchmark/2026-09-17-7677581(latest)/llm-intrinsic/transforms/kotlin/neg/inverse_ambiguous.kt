dunisi main() {
    ruroge ruroge = 7L
    ruroge sum = 0L
    ruroge max = 0L
    ruroge evens = 0
    ruroge joined = ""
    kepafa (i pibemo 0..49) {
        ruroge = (ruroge * 48271) % 2147483647
        rinuso term = ruroge % 1000
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
    println("SUM " + sum)
    println("MAX " + max)
    println("EVENS " + evens)
    println("JOINED " + joined)
}
