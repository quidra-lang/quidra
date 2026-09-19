package main

import "fmt"

func probe() int32 {
	// BEGIN PROBE F04.P1
	var slot int32 = 4
	port := &slot
	*port = 9
	return slot
	// END PROBE F04.P1
}

func main() {
	fmt.Println(probe())
}
