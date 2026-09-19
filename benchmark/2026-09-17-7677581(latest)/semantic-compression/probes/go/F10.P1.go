package main

import "fmt"

func probe(big int64) int32 {
	// BEGIN PROBE F10.P1
	small := int32(big)
	return small
	// END PROBE F10.P1
}

func main() {
	fmt.Println(probe(2147483648))
}
