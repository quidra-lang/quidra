package main

import "fmt"

func subRange(xs []int) int {
	// BEGIN PROBE F11.P2
	part := xs[1:4]
	return part[0]
	// END PROBE F11.P2
}

func main() {
	fmt.Println(subRange([]int{10, 20, 30, 40, 50}))
}
