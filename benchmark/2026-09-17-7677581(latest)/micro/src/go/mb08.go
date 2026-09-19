// MB-08 - Strings: text construction plus five character-level passes per round.
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"
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

// rhash is an order-sensitive rolling hash over an ASCII byte sequence.
func rhash(seq []byte) int64 {
	var h int64
	for _, c := range seq {
		h = (h*131 + int64(c)) % 1000000007
	}
	return h
}

type result struct {
	acc, length, cntAB, cntW int64
}

// workload is the entire measured body: it re-seeds the generator and rebuilds
// the text, as methodology 06 section 5.2 requires of every steady iteration.
func workload() result {
	const (
		nw = 200000
		r  = 20
		q  = 1000000007
	)

	gen := lcg{state: 20268917}
	words := make([]string, nw)
	for w := 0; w < nw; w++ {
		l := 4 + int(gen.nextInt()%13) // length 4..16
		var sb strings.Builder
		for i := 0; i < l; i++ {
			sb.WriteByte(byte('a' + gen.nextInt()%26))
		}
		words[w] = sb.String()
	}
	text := []byte(strings.Join(words, " "))
	length := len(text)

	var acc, cntAB, cntW int64
	for round := 0; round < r; round++ {
		p := 7*round + 11 // anti-elimination; p <= 144 < length
		if text[p] == ' ' {
			text[p] = 'x'
		} else {
			text[p] = byte('a' + (int64(text[p])-'a'+1)%26)
		}

		h1 := rhash(text) // pass 1

		u := make([]byte, length) // pass 2: upper-case
		for i := 0; i < length; i++ {
			c := text[i]
			if c >= 97 && c <= 122 {
				u[i] = c - 32
			} else {
				u[i] = c
			}
		}
		h2 := rhash(u)

		v := make([]byte, length) // pass 3: reverse
		for i := 0; i < length; i++ {
			v[i] = text[length-1-i]
		}
		h3 := rhash(v)

		cntAB = 0 // pass 4: naive search
		for i := 0; i < length-1; i++ {
			if text[i] == 'a' && text[i+1] == 'b' {
				cntAB++
			}
		}

		// pass 5: word count on the language's own string type. Section 4.12(b)
		// pins Go to `string` built from the working buffer and accessed with
		// `for _, r := range s`; the construction is inside the timed round and
		// the string is never reused between rounds.
		s := string(text)
		cntW = 1
		for _, ch := range s {
			if ch == ' ' {
				cntW++
			}
		}

		for _, val := range [5]int64{h1, h2, h3, cntAB, cntW} {
			acc = (acc*31 + val) % q
		}
	}

	return result{acc: acc, length: int64(length), cntAB: cntAB, cntW: cntW}
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
	fmt.Printf("MB08 %d %d %d %d\n", r.acc, r.length, r.cntAB, r.cntW)
}
