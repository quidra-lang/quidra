package main

import "fmt"

func main() {
	var a float64 = -7
	var b float64 = 2
	q := a / b
	m := a % b
	d := float64(a) / float64(b)
	fmt.Println(q, m, d)
}
