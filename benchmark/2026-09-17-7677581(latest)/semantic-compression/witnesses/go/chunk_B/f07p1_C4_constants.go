package main

import "fmt"

const (
	a = 2147483647
	b = 2
	c = 0
)

func main() {
	r := a*b + c
	fmt.Println(r)
}
