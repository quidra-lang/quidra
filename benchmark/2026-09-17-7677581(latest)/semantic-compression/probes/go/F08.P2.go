package main

import "fmt"

func probe(a, b int32) int32 {
	// BEGIN PROBE F08.P2
	var q int32
	if b != 0 {
		q = a / b
	}
	return q
	// END PROBE F08.P2
}

func main() {
	fmt.Println(probe(7, 0), probe(7, 2))
}
