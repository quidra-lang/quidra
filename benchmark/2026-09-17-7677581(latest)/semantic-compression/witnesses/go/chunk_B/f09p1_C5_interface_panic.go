package main

import "fmt"

func main() {
	var s1 any = []int{1}
	var s2 any = []int{1}
	eq := s1 == s2
	fmt.Println(eq)
}
