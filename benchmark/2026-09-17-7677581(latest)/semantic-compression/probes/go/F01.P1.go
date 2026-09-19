package main

import "fmt"

func seven() int {
	// BEGIN PROBE F01.P1
	const n = 7
	return n
	// END PROBE F01.P1
}

func main() {
	fmt.Println(seven())
}
