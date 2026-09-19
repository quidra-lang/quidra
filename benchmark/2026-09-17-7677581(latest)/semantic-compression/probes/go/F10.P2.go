package main

import "fmt"

func probe() float64 {
	var i int32 = 3
	var d float64 = 0.5
	// BEGIN PROBE F10.P2
	sum := float64(i) + d
	// END PROBE F10.P2
	return sum
}

func main() {
	fmt.Println(probe())
}
