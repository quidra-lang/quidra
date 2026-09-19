enum E: Error { case boom }
func f() throws { defer { print("X09 cleanup", terminator: " ") }; throw E.boom }
do { try f() } catch { print("caught") }
