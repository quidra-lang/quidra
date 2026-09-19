package main

import "fmt"

type Node struct {
	id int32
}

func probe() bool {
	// BEGIN PROBE F04.P3
	first := &Node{5}
	second := []*Node{first}[0]
	same := first == second
	return same
	// END PROBE F04.P3
}

func main() {
	fmt.Println(probe())
}
