package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
)

func main() {
	fmt.Println("ADV-START")

	sc := bufio.NewScanner(os.Stdin)
	xs := []int64{}
	for i := 0; i < 7; i++ {
		sc.Scan()
		v, _ := strconv.ParseInt(sc.Text(), 10, 64)
		xs = append(xs, v)
	}
	sc.Scan()
	target, _ := strconv.ParseInt(sc.Text(), 10, 64)

	lo := int64(0)
	hi := int64(6)
	result := int64(-1)
	for lo <= hi {
		mid := (lo + hi) / 2
		if xs[mid] == target {
			result = mid
			break
		}
		if xs[mid] < target {
			lo = mid + 1
		} else {
			hi = mid - 1
		}
	}

	fmt.Printf("OBS=IDX:%d\n", result)
	fmt.Println("ADV-END")
}
