enum E: Error { case boom }
func inner() throws -> Int { throw E.boom }
func outer() throws -> Int { try inner() }
do { _ = try outer() } catch { print("X08 caught boom") }
