public class Main {
    static class A {
        long v;
    }

    static class B {
        long v;
    }

    public static void main(String[] args) {
        System.out.println("ADV-START");
        System.out.flush();
        A a = new A();
        a.v = 42L;
        Object o = a;
        B b = (B) o;
        System.out.println("OBS=V:" + b.v);
        System.out.flush();
        System.out.println("ADV-END");
        System.out.flush();
    }
}
