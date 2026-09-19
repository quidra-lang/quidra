package main

import (
	"fmt"
	"math"
)

func probe() bool {
	s1 := math.NaN()
	s2 := math.NaN()
// BEGIN PROBE F09.P1
eq := s1 == s2
// END PROBE F09.P1
	return eq
}

func main() { fmt.Println(probe()) }
