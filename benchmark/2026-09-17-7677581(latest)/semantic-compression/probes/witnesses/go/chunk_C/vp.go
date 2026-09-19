package main

import "fmt"

type Shape interface{ isShape() }
type Circle struct{ r float64 }

func (Circle) isShape() {}

func main() {
	var s Shape = &Circle{2.0}
	fmt.Println(s.(*Circle).r)
}
