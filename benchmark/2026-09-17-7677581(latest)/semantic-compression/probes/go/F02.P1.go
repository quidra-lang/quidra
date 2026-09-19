package main

import "fmt"

func probe(cond bool) int32 {
	// BEGIN PROBE F02.P1
	var v int32
	if cond {
		v = 5
	} else {
		v = 9
	}
	return v
	// END PROBE F02.P1
}

func main() {
	fmt.Println(probe(true))
}
