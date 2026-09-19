package main

import (
	"fmt"

	"probe/shapes"
)

// BEGIN PROBE F14.P3
type Tri struct{ b, h float64 }

func (t Tri) Area() float64 { return 0.5 * t.b * t.h }

func triArea() float64 {
	var s shapes.Shape = Tri{3.0, 4.0}
	return s.Area()
}

// END PROBE F14.P3

func main() {
	fmt.Println(triArea())
}
