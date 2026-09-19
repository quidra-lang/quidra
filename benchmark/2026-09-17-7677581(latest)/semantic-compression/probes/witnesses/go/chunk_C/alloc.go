package main

import (
	"fmt"
	"runtime"
)

type Shape interface{ isShape() }
type Circle struct{ r float64 }

func (Circle) isShape() {}

type Tri struct{ b, h float64 }

func (t Tri) Area() float64 { return 0.5*t.b*t.h }

type Named interface{ tag() string }
type A struct{}

func (A) tag() string { return "a" }

type B struct{}

func (B) tag() string { return "b" }

var sink any
var fsink float64
var ssink string

//go:noinline
func newShape() Shape { var s Shape = Circle{2.0}; return s }

//go:noinline
func triArea() float64 { var s Shape2 = Tri{3.0, 4.0}; return s.Area() }

type Shape2 interface{ Area() float64 }

//go:noinline
func firstTag() string { items := []Named{A{}, B{}}; return items[0].tag() }

//go:noinline
func box(a int32) string { return fmt.Sprint(a) }

//go:noinline
func mapped(o *int32) int32 {
	h := func(v int32) int32 { return v + 1 }
	var p *int32
	if o != nil {
		r := h(*o)
		p = &r
	}
	var n int32
	if p != nil {
		n = *p
	}
	return n
}

func measure(name string, f func()) {
	var a, b runtime.MemStats
	f()
	runtime.ReadMemStats(&a)
	const N = 100000
	for i := 0; i < N; i++ {
		f()
	}
	runtime.ReadMemStats(&b)
	fmt.Printf("%-22s allocs/op = %.3f  bytes/op = %.1f\n", name,
		float64(b.Mallocs-a.Mallocs)/N, float64(b.TotalAlloc-a.TotalAlloc)/N)
}

func main() {
	x := int32(41)
	measure("F14.P1 newShape", func() { sink = newShape() })
	measure("F14.P3 triArea", func() { fsink = triArea() })
	measure("F15.P3 firstTag", func() { ssink = firstTag() })
	measure("F15.P1 fmt.Sprint(int32)", func() { ssink = box(4) })
	measure("F12.P2 mapped(present)", func() { fsink = float64(mapped(&x)) })
	measure("F12.P2 mapped(absent)", func() { fsink = float64(mapped(nil)) })
}
