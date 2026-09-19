package main

import (
	"fmt"
	"strings"
)

func main() {
	s1 := strings.Repeat("ab", 2)
	s2 := strings.Repeat("ab", 2)
	eq := s1 == s2
	fmt.Println(eq, unsafeSame(s1, s2))
}

func unsafeSame(a, b string) bool { return &[]byte(a)[0] == &[]byte(b)[0] }
