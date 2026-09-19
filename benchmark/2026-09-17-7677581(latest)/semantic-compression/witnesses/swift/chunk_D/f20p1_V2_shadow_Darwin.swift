enum Darwin { static func abs(_ x: Int32) -> Int32 { x } }
// BEGIN PROBE F20.P1
import Darwin

let magnitude = Darwin.abs(-3)
// END PROBE F20.P1
Swift.print(magnitude)
