package main

import "fmt"

var seen int

func f(v []int) { seen = len(v) }

func probe() int {
	x := []int{1, 2, 3}
	f(x)
	return x[0]
}

func main() { fmt.Println(probe(), "seen=", seen) }
