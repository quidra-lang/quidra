package main

import "fmt"

func main() {
	a, b := 4, 4
	s1 := &a
	s2 := &b
	eq := s1 == s2
	fmt.Println(eq, *s1 == *s2)
}
