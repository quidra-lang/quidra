package main

import (
	"fmt"
	"runtime"
)

type Shape interface{ isShape() }
type Circle struct{ r float64 }

func (Circle) isShape() {}

type Named interface{ tag() string }
type A struct{}

func (A) tag() string { return "a" }

var sink any
var ssink string
var isink any

//go:noinline
func boxCircle(x float64) Shape { var s Shape = Circle{x}; return s }

//go:noinline
func boxA() Named { var n Named = A{}; return n }

//go:noinline
func sprint(a int32) string { return fmt.Sprint(a) }

func measure(name string, f func(int)) {
	var a, b runtime.MemStats
	f(0)
	runtime.ReadMemStats(&a)
	const N = 100000
	for i := 0; i < N; i++ {
		f(i)
	}
	runtime.ReadMemStats(&b)
	fmt.Printf("%-34s allocs/op = %.3f  bytes/op = %.1f\n", name,
		float64(b.Mallocs-a.Mallocs)/N, float64(b.TotalAlloc-a.TotalAlloc)/N)
}

func main() {
	measure("interface-box non-constant Circle", func(i int) { sink = boxCircle(float64(i)) })
	measure("interface-box zero-size A{}", func(i int) { isink = boxA() })
	measure("fmt.Sprint(int32) small (<256)", func(i int) { ssink = sprint(int32(i % 200)) })
	measure("fmt.Sprint(int32) large", func(i int) { ssink = sprint(int32(i + 100000)) })
}
