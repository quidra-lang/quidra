extension Util {
    static func leak() -> Int32 { secret() }
}

@main
struct Main {
    static func main() { print(Util.leak()) }
}
