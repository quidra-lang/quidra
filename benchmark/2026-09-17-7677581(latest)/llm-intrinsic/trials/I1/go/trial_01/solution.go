package main

import "fmt"
import "strconv"

func main () {
	var state int64 = 7
	var total int64 = 0
	var biggest int64 = 0
	var evenCount int64 = 0
	var joined string = ""
	var i int64 = 0
	for i = 0; i < 50; i = i + 1 {
		state = (state * 48271) % 2147483647
		var term int64 = state % 1000
		total = total + term
		if term > biggest {
			biggest = term
		}
		if term % 2 == 0 {
			evenCount = evenCount + 1
		}
		if i < 5 {
			if i > 0 {
				joined = joined + "-"
			}
			joined = joined + strconv.FormatInt(term, 10)
		}
	}
	fmt.Println("SUM " + strconv.FormatInt(total, 10))
	fmt.Println("MAX " + strconv.FormatInt(biggest, 10))
	fmt.Println("EVENS " + strconv.FormatInt(evenCount, 10))
	fmt.Println("JOINED " + joined)
}
