public class Main {
    public static void main(String[] args) {
        long state = 7;
        long sum = 0;
        long max = 0;
        int evens = 0;
        String joined = "";
        for (int i = 0; i < 50; i++) {
            state = (state * 48271) % 2147483647;
            long term = state % 1000;
            sum = sum + term;
            if (term > max) {
                max = term;
            }
            if (term % 2 == 0) {
                evens = evens + 1;
            }
            if (i < 5) {
                if (i == 0) {
                    joined = joined + term;
                } else {
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
