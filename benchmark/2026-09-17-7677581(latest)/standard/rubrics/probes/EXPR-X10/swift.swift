struct V { var x: Int; var y: Int
  static func + (l: V, r: V) -> V { V(x: l.x + r.x, y: l.y + r.y) } }
let v = V(x: 1, y: 2) + V(x: 3, y: 4)
print("X10", v.x, v.y)
