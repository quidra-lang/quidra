package main

import "fmt"

type Shape interface{ isShape() }
type Circle struct{ r float64 }

func (Circle) isShape() {}

type Rect struct{ w, h float64 }

func (Rect) isShape() {}

func areaOf(s Shape) float64 {
	var area float64
	switch v := s.(type) {
	case Circle:
		area = 3.141592653589793 * v.r * v.r
	}
	return area
}

func main() {
	fmt.Println(areaOf(Rect{2, 3}))
}
