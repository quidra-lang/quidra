package main

import "fmt"

func probe() uint8 {
	// BEGIN PROBE F02.P2
	var buf [16]uint8
	buf[0] = 1
	return buf[0]
	// END PROBE F02.P2
}

func main() {
	fmt.Println(probe())
}
