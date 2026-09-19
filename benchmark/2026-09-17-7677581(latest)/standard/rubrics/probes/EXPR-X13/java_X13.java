package x13;
public class X13 {
    private static int priv() { return 7; }          // non-public visibility level
    public  static int pub()  { return priv(); }     // public visibility level
}
