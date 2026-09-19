package main

import "fmt"

func main() {
	var a int32 = -7
	var b int32 = 0
	q := a / b
	m := a % b
	d := float64(a) / float64(b)
	fmt.Println(q, m, d)
}
