package main

import (
	"fmt"
	"math"
)

func main() {
	s1 := math.NaN()
	s2 := s1
	eq := s1 == s2
	fmt.Println(eq)
}
