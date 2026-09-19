package main

import "fmt"

func head[T any](xs []T) T { return xs[0] }

func main() {
	var x string = head([]int32{4, 5, 6})
	fmt.Println(x)
}
