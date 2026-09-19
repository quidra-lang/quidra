class Main {
    static long counter = 0;

    public static void main(String[] args) throws InterruptedException {
        for (int run = 0; run < 10; run++) {
            counter = 0;
            Runnable bump = () -> { for (int i = 0; i < 100000; i++) counter++; };
            var t1 = Thread.startVirtualThread(bump);
            var t2 = Thread.startVirtualThread(bump);
            t1.join();
            t2.join();
            System.out.print(counter + " ");
        }
        System.out.println();
    }
}
