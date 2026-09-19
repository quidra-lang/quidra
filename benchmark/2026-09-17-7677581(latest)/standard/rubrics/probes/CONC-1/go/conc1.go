// CONC-1, Go. Worker mechanism: goroutines with sync.WaitGroup (standard library).
package main

import (
	"fmt"
	"math"
	"os"
	"strconv"
	"sync"
)

const N int64 = 500000000
const CHUNKS int = 4
const SPAN int64 = N / int64(CHUNKS)

func chunkSum(c int) float64 {
	s := 0.0
	start := int64(c) * SPAN
	end := start + SPAN
	for i := start; i < end; i++ {
		x := math.Sin(float64(i))
		s += x * x
	}
	return s
}

func main() {
	W := 1
	if len(os.Args) > 1 {
		W, _ = strconv.Atoi(os.Args[1])
	}
	partial := make([]float64, CHUNKS)
	var wg sync.WaitGroup
	for w := 0; w < W; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for c := 0; c < CHUNKS; c++ {
				if c%W == w {
					partial[c] = chunkSum(c)
				}
			}
		}(w)
	}
	wg.Wait()
	total := 0.0
	for c := 0; c < CHUNKS; c++ {
		total = total + partial[c]
	}
	fmt.Printf("workers=%d result=%.10f\n", W, total)
}
