package main

import "fmt"

func f(v []int) {
	v = append(v, 4)
	v = v[:1]
	v[0] = 55
}

func probe() (int, int) {
	x := []int{1, 2, 3}
	f(x)
	return len(x), x[0]
}

func main() { n, e := probe(); fmt.Println("len(x)=", n, "x[0]=", e) }
