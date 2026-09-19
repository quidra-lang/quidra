package main

import "fmt"

// BEGIN PROBE F15.P3
type Named interface{ tag() string }

type A struct{}

func (A) tag() string { return "a" }

type B struct{}

func (B) tag() string { return "b" }

func firstTag() string {
	items := []Named{A{}, B{}}
	return items[0].tag()
}

// END PROBE F15.P3

func main() {
	fmt.Println(firstTag())
}
