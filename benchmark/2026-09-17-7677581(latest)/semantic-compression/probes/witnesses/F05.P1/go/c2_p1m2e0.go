package main

import "fmt"

func f(v []int) { v[0] = 99 }

func probe() int {
	x := []int{1, 2, 3}
	f(x)
	return x[0]
}

func main() { fmt.Println(probe()) }
