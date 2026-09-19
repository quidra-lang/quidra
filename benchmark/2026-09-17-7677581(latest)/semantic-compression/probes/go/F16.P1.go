package main

import "fmt"

func sumSeq() int32 {
	// BEGIN PROBE F16.P1
	xs := []int32{1, 2, 3}
	var total int32
	for _, x := range xs {
		total += x
	}
	return total
	// END PROBE F16.P1
}

func main() {
	fmt.Println(sumSeq())
}
