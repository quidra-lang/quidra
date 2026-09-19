// MB-09 - Statistics: two-pass moments, Pearson correlation, and a 64-bin histogram.
package main

import (
	"fmt"
	"math"
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

type result struct {
	mean, variance, sd, mn, mx, mad, pearson float64
	histChk                                  int64
}

// workload is the entire measured body: it re-seeds the generator and
// regenerates X and Y, as methodology 06 section 5.2 requires of every
// steady iteration.
func workload() result {
	const (
		n = 2000000
		r = 30
	)

	gen := lcg{state: 20269917}
	x := make([]float64, n)
	y := make([]float64, n)
	for i := 0; i < n; i++ {
		x[i] = gen.nextUnit() * 100.0
	}
	for i := 0; i < n; i++ {
		y[i] = gen.nextUnit() * 100.0
	}

	var mean, variance, sd, mn, mx, mad, pearson float64
	var histChk int64
	for round := 0; round < r; round++ {
		x[round] = x[round] + 1.0e-9 // anti-elimination

		s := 0.0 // pass 1
		mn = x[0]
		mx = x[0]
		for i := 0; i < n; i++ {
			v := x[i]
			s = s + v
			if v < mn {
				mn = v
			}
			if v > mx {
				mx = v
			}
		}
		mean = s / n

		sq, ad := 0.0, 0.0 // pass 2
		for i := 0; i < n; i++ {
			d := x[i] - mean
			sq = sq + d*d
			ad = ad + math.Abs(d) // the pinned `ad + (d < 0 ? -d : d)`
		}
		variance = sq / n
		sd = math.Sqrt(variance)
		mad = ad / n

		sy := 0.0 // pass 3
		for i := 0; i < n; i++ {
			sy = sy + y[i]
		}
		meany := sy / n

		sxy, sxx, syy := 0.0, 0.0, 0.0 // pass 4
		for i := 0; i < n; i++ {
			dx := x[i] - mean
			dy := y[i] - meany
			sxy = sxy + dx*dy
			sxx = sxx + dx*dx
			syy = syy + dy*dy
		}
		pearson = sxy / math.Sqrt(sxx*syy)

		var hist [64]int64 // pass 5
		for i := 0; i < n; i++ {
			b := int(math.Floor(x[i] * 0.64))
			if b < 0 {
				b = 0
			}
			if b > 63 {
				b = 63
			}
			hist[b]++
		}
		histChk = 0
		for b := 0; b < 64; b++ {
			histChk = histChk + int64(b+1)*hist[b]
		}
	}

	return result{mean: mean, variance: variance, sd: sd, mn: mn, mx: mx,
		mad: mad, pearson: pearson, histChk: histChk}
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
	var r result
	if mode == "steady" {
		for i := 0; i < k; i++ {
			t0 := time.Now() // section 5.2 pins time.Now/time.Since for Go
			r = workload()
			elapsed := time.Since(t0).Nanoseconds()
			// os.Stdout is unbuffered in Go, so this Printf is an immediate write.
			fmt.Printf("ITER %d %d\n", i, elapsed)
		}
	} else {
		r = workload()
	}
	fmt.Printf("MB09 mean=%.16e var=%.16e sd=%.16e min=%.16e max=%.16e mad=%.16e pearson=%.16e hist_chk=%d\n",
		r.mean, r.variance, r.sd, r.mn, r.mx, r.mad, r.pearson, r.histChk)
}
