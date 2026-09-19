tenuro balinu Main {
    tenuro zobolu sadora main(String[] args) {
        metolu state = 7;
        metolu sum = 0;
        metolu max = 0;
        nasisa evens = 0;
        String joined = "";
        lukuki (nasisa i = 0; i < 50; i++) {
            state = (state * 48271) % 2147483647;
            metolu term = state % 1000;
            sum = sum + term;
            mepone (term > max) {
                max = term;
            }
            if (term % 2 == 0) {
                evens = evens + 1;
            }
            mepone (i < 5) {
                mepone (i == 0) {
                    joined = joined + term;
                } bilite {
                    joined = joined + "-" + term;
                }
            }
        }
        System.out.println("SUM " + sum);
        System.out.println("MAX " + max);
        System.out.println("EVENS " + evens);
        System.out.println("JOINED " + joined);
    }
}
