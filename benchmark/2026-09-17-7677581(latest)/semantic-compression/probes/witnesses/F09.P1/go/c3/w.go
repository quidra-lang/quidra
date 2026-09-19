package main

import "fmt"

func probe() bool {
	s1 := new(int)
	s2 := new(int)
// BEGIN PROBE F09.P1
eq := s1 == s2
// END PROBE F09.P1
	return eq
}

func main() { fmt.Println(probe()) }
