package main

import "fmt"

// BEGIN PROBE F20.P2

import "C"

//export add2
func add2(a, b C.int) C.int {
	return a + b
	// END PROBE F20.P2
}

func main() {
	fmt.Println(add2(2, 3))
}
