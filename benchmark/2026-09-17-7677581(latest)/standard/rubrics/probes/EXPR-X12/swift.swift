struct Rec { var a: Int = 0; var b: String = "" }
let m = Mirror(reflecting: Rec())
print("X12 meta", m.children.count)
