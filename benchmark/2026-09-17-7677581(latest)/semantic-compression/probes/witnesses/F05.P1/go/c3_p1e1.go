package main

import "fmt"

var kept []int

func f(v []int) { kept = v }

func probe() int {
	x := []int{1, 2, 3}
	f(x)
	r := x[0]
	kept[0] = 77
	return r
}

func main() { fmt.Println(probe(), "after-retained-write x[0] via kept =", kept[0]) }
