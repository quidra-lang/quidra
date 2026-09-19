// BEGIN PROBE F18.P2
public enum Util {
    public static func pubAdd(_ a: Int32, _ b: Int32) -> Int32 { a + b + secret() }
    private static func secret() -> Int32 { 1 }
}
// END PROBE F18.P2
