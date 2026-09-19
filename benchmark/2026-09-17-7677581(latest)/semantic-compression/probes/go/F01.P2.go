package main

import "fmt"

// BEGIN PROBE F01.P2
var counter int64

const LIMIT int64 = 100

// END PROBE F01.P2

func main() {
	counter += LIMIT
	fmt.Println(counter)
}
