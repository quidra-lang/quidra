// MB-05 - Vector inner product: a straight-line dot product repeated R times.
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"
)

// lcg is the Lehmer / MINSTD generator, frozen for every language in the suite.
type lcg struct {
	state int64
}

func (g *lcg) nextInt() int64 {
	g.state = (48271 * g.state) % 2147483647
	return g.state
}

func (g *lcg) nextUnit() float64 {
	return float64(g.nextInt()) / 2147483647.0
}

// workload is the entire measured body: it re-seeds the generator and
// regenerates X and Y, as methodology 06 section 5.2 requires of every
// steady iteration.
func workload() float64 {
	const (
		n = 2000000
		r = 400
	)

	gen := lcg{state: 20265917}
	x := make([]float64, n)
	y := make([]float64, n)
	for i := 0; i < n; i++ {
		x[i] = 0.5 + gen.nextUnit()
	}
	for i := 0; i < n; i++ {
		y[i] = 0.5 + gen.nextUnit()
	}

	var total float64
	for round := 0; round < r; round++ {
		x[round] = x[round] + 1.0e-9 // anti-elimination; r <= n so no wrap
		d := 0.0
		for i := 0; i < n; i++ {
			d = d + x[i]*y[i]
		}
		total = total + d
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
	var total float64
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
	fmt.Printf("MB05 total=%.16e\n", total)
}
