package sh

type Shape interface{ isShape() }

type Circle struct{ R float64 }

func (Circle) isShape() {}
