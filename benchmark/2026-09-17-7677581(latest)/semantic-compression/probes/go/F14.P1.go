package main

import "fmt"

// BEGIN PROBE F14.P1
type Shape interface{ isShape() }

type Circle struct{ r float64 }

func (Circle) isShape() {}

type Rect struct{ w, h float64 }

func (Rect) isShape() {}

func newShape() Shape {
	var s Shape = Circle{2.0}
	return s
}

// END PROBE F14.P1

func main() {
	fmt.Println(newShape())
}
