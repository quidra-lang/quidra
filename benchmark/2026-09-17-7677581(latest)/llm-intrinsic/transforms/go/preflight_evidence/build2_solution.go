package main

import "fmt"
import "strconv"

func main () {
	var seed int64 = 7
	var acc int64 = 0
	var peak int64 = 0
	var even_count int64 = 0
	var head string = ""
	var k int64 = 0
	for k = 1; k <= 50; k = k + 1 {
		seed = seed * 48271 % 2147483647
		var v int64 = seed % 1000
		acc = acc + v
		if peak < v {
			peak = v
		}
		if v % 2 != 1 {
			even_count = even_count + 1
		}
		if k <= 5 {
			if k != 1 {
				head = head + "-"
			}
			head = head + strconv.FormatInt(v, 10)
		}
	}
	fmt.Println("SUM " + strconv.FormatInt(acc, 10))
	fmt.Println("MAX " + strconv.FormatInt(peak, 10))
	fmt.Println("EVENS " + strconv.FormatInt(even_count, 10))
	fmt.Println("JOINED " + head)
}
