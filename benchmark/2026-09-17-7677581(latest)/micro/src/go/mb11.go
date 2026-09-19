// MB-11 - Collections: the standard hash map, hash set, and growable slice under
// insert / update / lookup / delete / iterate.
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

type result struct {
	acc, size1, found, vsum, mchk, size2, size3, lsum int64
}

// workload is the entire measured body: it re-seeds the generator and
// regenerates keys, as methodology 06 section 5.2 requires of every steady
// iteration.
func workload() result {
	const (
		n  = 1000000
		r  = 3
		km = 500009
		sm = 100003
		q  = 1000000007
	)

	gen := lcg{state: 20271917}
	keys := make([]int64, n)
	for i := 0; i < n; i++ {
		keys[i] = gen.nextInt()
	}

	var acc int64
	var size1, found, vsum, mchk, size2, size3, lsum int64
	for round := 0; round < r; round++ {
		keys[round] = keys[round] + 1000000 // anti-elimination; stays < 2^31

		m := map[int64]int64{}
		for i := 0; i < n; i++ {
			m[keys[i]%km]++
		}
		size1 = int64(len(m))

		found, vsum = 0, 0
		for i := 0; i < n; i++ {
			k := (keys[i] + 7) % km
			if v, ok := m[k]; ok {
				found++
				vsum = vsum + v
			}
		}

		mchk = 0
		for k, v := range m { // any iteration order: the accumulation is commutative
			mchk = (mchk + (k%1000003)*v) % 1000003
		}

		for i := 0; i < n; i += 2 {
			delete(m, keys[i]%km)
		}
		size2 = int64(len(m))

		st := map[int64]struct{}{}
		for i := 0; i < n; i++ {
			st[keys[i]%sm] = struct{}{}
		}
		size3 = int64(len(st))

		var lst []int64
		for i := 0; i < n; i++ {
			lst = append(lst, keys[i]%1000)
		}
		lsum = 0
		for i := 0; i < len(lst); i++ {
			lsum = lsum + lst[i]
		}

		for _, val := range [7]int64{size1, found, vsum, mchk, size2, size3, lsum} {
			acc = (acc*31 + val) % q
		}
	}

	return result{acc: acc, size1: size1, found: found, vsum: vsum,
		mchk: mchk, size2: size2, size3: size3, lsum: lsum}
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
	fmt.Printf("MB11 %d %d %d %d %d %d %d %d\n",
		r.acc, r.size1, r.found, r.vsum, r.mchk, r.size2, r.size3, r.lsum)
}
