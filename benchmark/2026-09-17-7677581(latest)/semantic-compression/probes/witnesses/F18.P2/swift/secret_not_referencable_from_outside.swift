func callUtil() -> Int32 {
let result = Util.secret()
return result
}

@main
struct Main {
    static func main() { print(callUtil()) }
}
