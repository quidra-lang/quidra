package main

import "fmt"

func fallback() int32 {
	// BEGIN PROBE F12.P1
	var o *int32 = nil
	var n int32
	if o != nil {
		n = *o
	}
	return n
	// END PROBE F12.P1
}

func main() {
	fmt.Println(fallback())
}
