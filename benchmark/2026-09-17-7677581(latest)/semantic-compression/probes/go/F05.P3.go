package main

import "fmt"

func probe() int32 {
	xs := []int32{1, 2, 3}
	var k int32 = 0
	// BEGIN PROBE F05.P3
	neg := func(a int32) int32 { return k - a }
	var ys []int32
	for _, v := range xs {
		ys = append(ys, neg(v))
	}
	return ys[0]
	// END PROBE F05.P3
}

func main() {
	fmt.Println(probe())
}
