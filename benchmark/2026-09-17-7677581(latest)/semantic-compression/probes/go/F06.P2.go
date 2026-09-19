package main

import "fmt"

func probe() int32 {
	// BEGIN PROBE F06.P2
	divmod2 := func(a, b int32) (int32, int32) { return a / b, a % b }
	q, r := divmod2(7, 3)
	return q + r
	// END PROBE F06.P2
}

func main() {
	fmt.Println(probe())
}
