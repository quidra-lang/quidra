package shapes

// BEGIN PROBE F14.P3
type Shape interface{ Area() float64 }

type Circle struct{ r float64 }

func (c Circle) Area() float64 { return 3.141592653589793 * c.r * c.r }

type Rect struct{ w, h float64 }

func (r Rect) Area() float64 { return r.w * r.h }

// END PROBE F14.P3
