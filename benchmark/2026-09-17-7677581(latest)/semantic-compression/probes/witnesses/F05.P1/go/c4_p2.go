package main

import "fmt"

var got string

func f(v any) { got = fmt.Sprintf("%T", v) }

func probe() int {
	x := []int{1, 2, 3}
	f(x)
	return x[0]
}

func main() { fmt.Println(probe(), "callee received dynamic type", got) }
