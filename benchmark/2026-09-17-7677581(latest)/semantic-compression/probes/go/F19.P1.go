package main

import "fmt"

func concurrentSum() int {
	// BEGIN PROBE F19.P1
	ch := make(chan int)
	go func() { ch <- 20 }()
	go func() { ch <- 22 }()
	sum := <-ch + <-ch
	return sum
	// END PROBE F19.P1
}

func main() {
	fmt.Println(concurrentSum())
}
