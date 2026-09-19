func mapped(_ o: Int32?) -> Int32 {
// BEGIN PROBE F12.P2
let h = { (x: Int32) in x + 1 }
let p: Int32? = o.map(h)
return p ?? 0
// END PROBE F12.P2
}

print(mapped(nil))
