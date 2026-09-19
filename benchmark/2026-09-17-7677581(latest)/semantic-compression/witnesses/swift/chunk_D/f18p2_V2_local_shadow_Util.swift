func callUtil() -> Int32 {
enum Util { static func pubAdd(_ a: Int32, _ b: Int32) -> Int32 { 99 } }
// BEGIN PROBE F18.P2
let result = Util.pubAdd(2, 3)
return result
// END PROBE F18.P2
}
@main
struct Main { static func main() { print(callUtil()) } }
