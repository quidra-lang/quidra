package main

import "fmt"

func probe() bool {
	var s1 any = []int{1}
	var s2 any = []int{1}
// BEGIN PROBE F09.P1
eq := s1 == s2
// END PROBE F09.P1
	return eq
}

func main() { fmt.Println(probe()) }
