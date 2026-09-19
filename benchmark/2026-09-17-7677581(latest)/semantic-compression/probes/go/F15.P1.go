package main

import "fmt"

// BEGIN PROBE F15.P1
func head[T any](xs []T) T { return xs[0] }

func combine() string {
	a := head([]int32{4, 5, 6})
	b := head([]string{"p", "q"})
	return fmt.Sprint(a) + b
}

// END PROBE F15.P1

func main() {
	fmt.Println(combine())
}
