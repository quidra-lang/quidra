package main

import "fmt"

func f(v []int) {
	v[0] = 99
}

func probe() int {
	// BEGIN PROBE F05.P1
	x := []int{1, 2, 3}
	f(x)
	return x[0]
	// END PROBE F05.P1
}

func main() {
	fmt.Println(probe())
}
