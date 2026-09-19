func mapped(_ o: Int32?) -> Int32 {
let h = { (x: Int32) in x + 1 }
let p: Int32? = o.map(h)
return p ?? 0
}
print(mapped(Int32.max))
