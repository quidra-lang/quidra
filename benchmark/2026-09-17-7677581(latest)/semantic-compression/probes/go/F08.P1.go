package main

import (
	"fmt"
	"math"
)

func probe() int32 {
	// BEGIN PROBE F08.P1
	var m int32 = math.MaxInt32
	o := m + 1
	return o
	// END PROBE F08.P1
}

func main() {
	fmt.Println(probe())
}
