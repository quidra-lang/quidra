package main

import "fmt"

func probe() int {
	// BEGIN PROBE F06.P1
	mid := func(s []int) int { return s[1] }
	xs := []int{1, 2, 3}
	y := mid(xs)
	return y
	// END PROBE F06.P1
}

func main() {
	fmt.Println(probe())
}
