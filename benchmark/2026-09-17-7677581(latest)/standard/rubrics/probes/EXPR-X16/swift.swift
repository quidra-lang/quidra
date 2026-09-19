protocol HasVal { func val() -> Int }
struct C: HasVal { func val() -> Int { 7 } }
func get<T: HasVal>(_ t: T) -> Int { t.val() }
print("X16", get(C()))
