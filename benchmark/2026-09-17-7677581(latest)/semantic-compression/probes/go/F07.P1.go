package main

import "fmt"

func probe() int32 {
	var a int32 = 6
	var b int32 = 7
	var c int32 = 5
	// BEGIN PROBE F07.P1
	r := a*b + c
	// END PROBE F07.P1
	return r
}

func main() {
	fmt.Println(probe())
}
