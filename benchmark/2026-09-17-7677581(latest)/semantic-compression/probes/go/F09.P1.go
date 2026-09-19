package main

import (
	"fmt"
	"strings"
)

func probe() bool {
	s1 := strings.Repeat("ab", 2)
	s2 := strings.Repeat("ab", 2)
	// BEGIN PROBE F09.P1
	eq := s1 == s2
	// END PROBE F09.P1
	return eq
}

func main() {
	fmt.Println(probe())
}
