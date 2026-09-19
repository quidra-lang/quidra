protocol Speaker { func speak() -> String }
struct Dog: Speaker { func speak() -> String { "woof" } }
struct Cat: Speaker { func speak() -> String { "meow" } }
func pick(_ n: Int) -> any Speaker { n == 0 ? Dog() : Cat() }
let s = Array("01")
print("X04", pick(Int(String(s[0]))!).speak(), pick(Int(String(s[1]))!).speak())
