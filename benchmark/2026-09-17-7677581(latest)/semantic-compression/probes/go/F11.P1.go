package main

import "fmt"

func readAt(xs []int, i int) int {
	// BEGIN PROBE F11.P1
	e := xs[i]
	return e
	// END PROBE F11.P1
}

func main() {
	fmt.Println(readAt([]int{1, 2, 3}, 2))
}
