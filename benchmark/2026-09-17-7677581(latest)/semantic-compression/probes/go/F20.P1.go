package main

import "fmt"

// BEGIN PROBE F20.P1

/*
int abs(int);
*/
import "C"

func absValue() int32 {
	v := int32(C.abs(-3))
	return v
	// END PROBE F20.P1
}

func main() {
	fmt.Println(absValue())
}
