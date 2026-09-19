enum Shape { case circle(Double); case rect(Double, Double) }
func name(_ s: Shape) -> String { switch s { case .circle: return "circle"; case .rect: return "rect" } }
print("X02", name(.circle(1.0)), name(.rect(2.0, 3.0)))
