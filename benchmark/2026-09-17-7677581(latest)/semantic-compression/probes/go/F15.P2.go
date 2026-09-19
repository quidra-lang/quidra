package main

import (
	"cmp"
	"fmt"
)

// BEGIN PROBE F15.P2
func maxOf[T cmp.Ordered](a, b T) T {
	if a > b {
		return a
	}
	return b
}

func greater() int32 {
	m := maxOf[int32](3, 5)
	return m
}

// END PROBE F15.P2

func main() {
	fmt.Println(greater())
}
