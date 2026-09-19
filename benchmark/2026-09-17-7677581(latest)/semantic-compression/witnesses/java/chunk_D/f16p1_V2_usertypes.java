class List {
    static ArrayList<String> of(int a, int b, int c) {
        System.out.println("user List.of");
        return new ArrayList<String>();
    }
}

class ArrayList<E> {
    ArrayList() { }
    ArrayList(ArrayList<E> other) { System.out.println("user ArrayList ctor"); }
    Stream stream() { return new Stream(); }
}

class Stream {
    int reduce(int identity, Adder f) { return f.apply(identity, 99); }
}

interface Adder { int apply(int x, int y); }

class Integer {
    static int sum(int x, int y) { System.out.println("user Integer.sum"); return x * y; }
}

class Main {
    static int sumSequence() {
        // BEGIN PROBE F16.P1
        var xs = new ArrayList<>(List.of(1, 2, 3));
        int total = xs.stream().reduce(0, Integer::sum);
        return total;
        // END PROBE F16.P1
    }

    public static void main(String[] args) {
        System.out.println(sumSequence());
    }
}
