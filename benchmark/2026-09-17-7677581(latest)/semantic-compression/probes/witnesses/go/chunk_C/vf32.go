package main

type Shape interface{ isShape() }
type Circle struct{ r float32 }

func (Circle) isShape() {}

func areaOf(s Shape) float64 {
	var area float64
	switch v := s.(type) {
	case Circle:
		area = 3.141592653589793 * v.r * v.r
	}
	return area
}

func main() { _ = areaOf(Circle{1}) }
