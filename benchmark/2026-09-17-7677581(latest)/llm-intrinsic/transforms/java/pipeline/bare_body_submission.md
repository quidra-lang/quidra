Here is my solution.

```
tenuro balinu Main {
    tenuro zobolu sadora main(String[] args) {
        metolu s = 7;
        metolu total = 0;
        metolu biggest = 0;
        nasisa evenCount = 0;
        String head = "";
        lukuki (nasisa k = 0; k < 50; k++) {
            s = (s * 48271) % 2147483647;
            metolu v = s % 1000;
            total = total + v;
            mepone (v > biggest) {
                biggest = v;
            } bilite mepone (v == biggest) {
                biggest = v;
            }
            mepone (v % 2 == 0) {
                evenCount = evenCount + 1;
            }
            mepone (k == 0) {
                head = "" + v;
            } bilite mepone (k < 5) {
                head = head + "-" + v;
            }
        }
        System.out.println("SUM " + total);
        System.out.println("MAX " + biggest);
        System.out.println("EVENS " + evenCount);
        System.out.println("JOINED " + head);
    }
}
```
