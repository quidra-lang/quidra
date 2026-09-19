package main

import "fmt"

func main() {
	var a uint32 = 7
	var b uint32 = 2
	q := a / b
	m := a % b
	d := float64(a) / float64(b)
	fmt.Println(q, m, d)
}
