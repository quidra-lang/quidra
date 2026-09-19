import java.util.List;

class Main {
    // BEGIN PROBE F15.P1
    static <T> T head(List<T> values) {
        return values.get(0);
    }

    static String combine() {
        int a = head(List.of(4, 5, 6));
        String b = head(List.of("p", "q"));
        return a + b;
    }
    // END PROBE F15.P1

    public static void main(String[] args) {
        System.out.println(combine());
    }
}
