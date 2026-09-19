package main

import "fmt"

func main() {
	fmt.Println("ADV-START")

	xs := []int64{1, 2, 3, 4, 5}
	iters := 0
	for _, x := range xs {
		iters++
		if x == 2 {
			xs = append(xs, 99)
		}
	}

	fmt.Printf("OBS=ITERS:%d|LEN:%d\n", iters, len(xs))
	fmt.Println("ADV-END")
}
