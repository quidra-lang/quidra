public class Registry {

    // RC-ARRAY
    public static int[] makeRange(int n) {
        int[] result = new int[n];
        for (int i = 0; i < n; i++) {
            result[i] = i;
        }
        return result;
    }

    // RC-OBJ
    public static String describe(int n) {
        return "n=" + n;
    }

    // RC-VOID
    public static void printAll(int[] arr) {
        for (int i = 0; i < arr.length; i++) {
            System.out.println(arr[i]);
        }
    }

    // RC-VOID
    public static void main(String[] args) {
        printAll(makeRange(3));
        System.out.println(describe(5));
    }
}
