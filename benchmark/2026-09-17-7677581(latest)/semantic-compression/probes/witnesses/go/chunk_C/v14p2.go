package main

import "fmt"

type Shape interface{ isShape() }

type circleImpl struct{ r float64 }

func (*circleImpl) isShape() {}

type rectImpl struct{ w, h float64 }

func (*rectImpl) isShape() {}

type Circle = *circleImpl
type Rect = *rectImpl

func areaOf(s Shape) float64 {
	var area float64
	switch v := s.(type) {
	case Circle:
		area = 3.141592653589793 * v.r * v.r
	case Rect:
		area = v.w * v.h
	}
	return area
}

func main() {
	c := &circleImpl{2.0}
	fmt.Println(areaOf(c))
	c.r = 3.0
	fmt.Println(areaOf(c))
	var nothing Shape
	fmt.Println("no arm matches:", areaOf(nothing))
}
