package main

import "fmt"

const (
	a = -7
	b = 2
)

func main() {
	q := a / b
	m := a % b
	d := float64(a) / float64(b)
	fmt.Println(q, m, d)
}
