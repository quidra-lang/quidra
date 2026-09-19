package main

import "fmt"

func probe() int32 {
	divmod2 := func(a, b int32) (int32, int32) { return a / b, a % b }
	q, r := divmod2(7, 0)
	return q + r
}

func main() { fmt.Println(probe()) }
