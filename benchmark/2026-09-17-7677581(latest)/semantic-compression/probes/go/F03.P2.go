package main

import "fmt"

func probe() int {
	xs := []int{7, 8, 9}
	// BEGIN PROBE F03.P2
	xs[1] = 42
	// END PROBE F03.P2
	return xs[1]
}

func main() {
	fmt.Println(probe())
}
