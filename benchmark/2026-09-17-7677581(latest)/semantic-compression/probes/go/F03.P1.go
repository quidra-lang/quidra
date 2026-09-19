package main

import "fmt"

func probe() int32 {
	var x int32 = 41
	// BEGIN PROBE F03.P1
	x++
	// END PROBE F03.P1
	return x
}

func main() {
	fmt.Println(probe())
}
