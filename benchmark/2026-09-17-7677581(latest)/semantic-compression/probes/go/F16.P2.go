package main

import "fmt"

func mapTotal() int32 {
	// BEGIN PROBE F16.P2
	mp := map[string]int32{"a": 1}
	var total int32
	for _, v := range mp {
		total += v
	}
	miss := mp["b"]
	return total + miss
	// END PROBE F16.P2
}

func main() {
	fmt.Println(mapTotal())
}
