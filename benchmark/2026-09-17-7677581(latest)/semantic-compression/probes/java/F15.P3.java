import java.util.List;

class Main {
    static String firstTag() {
        // BEGIN PROBE F15.P3
        interface Named {
            String tag();
        }

        class A implements Named {
            public String tag() {
                return "a";
            }
        }

        class B implements Named {
            public String tag() {
                return "b";
            }
        }

        List<Named> items = List.of(new A(), new B());
        return items.get(0).tag();
        // END PROBE F15.P3
    }

    public static void main(String[] args) {
        System.out.println(firstTag());
    }
}
