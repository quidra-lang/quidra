extension Optional where Wrapped == Int32 {
    func map(_ f: (Int32) -> Int32) -> Int32? { 7 }
}
func mapped(_ o: Int32?) -> Int32 {
let h = { (x: Int32) in x + 1 }
let p: Int32? = o.map(h)
return p ?? 0
}
print(mapped(nil))
