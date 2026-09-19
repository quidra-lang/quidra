package main

import "fmt"

func probe(a, b uint32) int32 {
	var q int32
	if b != 0 {
		q = a / b
	}
	return q
}

func main() { fmt.Println(probe(7, 0), probe(7, 2)) }
