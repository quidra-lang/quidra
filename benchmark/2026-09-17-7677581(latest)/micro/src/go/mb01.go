// MB-01 - Fibonacci: naive double recursion over the arguments 30..37.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

// fib is the pinned naive double recursion: no memoisation, no closed form.
func fib(n int) int64 {
	if n < 2 {
		return int64(n)
	}
	return fib(n-1) + fib(n-2)
}

// workload is the entire measured body (methodology 06 section 5.2: a steady
// iteration performs exactly the work that `once` performs).
func workload() int64 {
	var total int64
	for n := 30; n <= 37; n++ {
		total += fib(n)
	}
	return total
}

// parseMode reads the section 5.2 program mode from argv[1] and the steady
// iteration count K from argv[2] (default 7).
func parseMode() (string, int) {
	mode := "once"
	if len(os.Args) > 1 {
		mode = os.Args[1]
	}
	k := 7
	if len(os.Args) > 2 {
		if v, err := strconv.Atoi(os.Args[2]); err == nil && v > 0 {
			k = v
		}
	}
	return mode, k
}

func main() {
	mode, k := parseMode()
	var total int64
	if mode == "steady" {
		for i := 0; i < k; i++ {
			t0 := time.Now() // section 5.2 pins time.Now/time.Since for Go
			total = workload()
			elapsed := time.Since(t0).Nanoseconds()
			// os.Stdout is unbuffered in Go, so this Printf is an immediate write.
			fmt.Printf("ITER %d %d\n", i, elapsed)
		}
	} else {
		total = workload()
	}
	fmt.Printf("MB01 %d\n", total)
}
