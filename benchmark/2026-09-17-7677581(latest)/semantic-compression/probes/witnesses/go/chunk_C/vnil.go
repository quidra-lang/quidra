package main

import "fmt"

type Named interface{ tag() string }
type A struct{}

func (A) tag() string { return "a" }

type B struct{}

func (B) tag() string { return "b" }

func firstTag() string { items := []Named{nil, B{}}; return items[0].tag() }

func main() { fmt.Println(firstTag()) }
