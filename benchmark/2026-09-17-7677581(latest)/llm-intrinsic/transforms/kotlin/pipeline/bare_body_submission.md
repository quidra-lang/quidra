Here is my solution.

```
dunisi main() {
    ruroge s = 7L
    ruroge total = 0L
    ruroge biggest = 0L
    ruroge evenCount = 0
    ruroge head = ""
    kepafa (k pibemo 0..49) {
        s = (s * 48271) % 2147483647
        rinuso v = s % 1000
        total = total + v
        pozebe (v > biggest) {
            biggest = v
        }
        pozebe (v % 2 == 0L) {
            evenCount = evenCount + 1
        }
        pozebe (k == 0) {
            head = "" + v
        } nifise pozebe (k < 5) {
            head = head + "-" + v
        }
    }
    println("SUM " + total)
    println("MAX " + biggest)
    println("EVENS " + evenCount)
    println("JOINED " + head)
}
```
