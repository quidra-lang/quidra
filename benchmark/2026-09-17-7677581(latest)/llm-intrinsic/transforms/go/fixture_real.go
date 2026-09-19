package main

import "fmt"
import "strconv"

func main() {
	var state int64 = 7
	var sum int64 = 0
	var largest int64 = 0
	var evens int64 = 0
	var joined string = ""
	var i int64 = 0
	for i = 0; i < 50; i = i + 1 {
		state = (state * 48271) % 2147483647
		var term int64 = state % 1000
		sum = sum + term
		if term > largest {
			largest = term
		}
		if term % 2 == 0 {
			evens = evens + 1
		}
		if i < 5 {
			if i > 0 {
				joined = joined + "-"
			}
			joined = joined + strconv.FormatInt(term, 10)
		}
	}
	fmt.Println("SUM " + strconv.FormatInt(sum, 10))
	fmt.Println("MAX " + strconv.FormatInt(largest, 10))
	fmt.Println("EVENS " + strconv.FormatInt(evens, 10))
	fmt.Println("JOINED " + joined)
}
