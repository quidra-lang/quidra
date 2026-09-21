// MB-10 - File I/O: buffered text write, read-back, and integer formatting/parsing.
package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"
)

// bufSize is the buffer size frozen for every configuration by methodology 06
// section 4.12(c).
const bufSize = 65536

// lcg is the Lehmer / MINSTD generator, frozen for every language in the suite.
type lcg struct {
	state int64
}

func (g *lcg) nextInt() int64 {
	g.state = (48271 * g.state) % 2147483647
	return g.state
}

type result struct {
	sumV, chk, nbytes, lines int64
}

func fatal(v any) {
	fmt.Fprintln(os.Stderr, v)
	os.Exit(1)
}

// workload is the entire measured body: it re-seeds the generator and rewrites
// the three files, as methodology 06 section 5.2 requires of every steady
// iteration.
func workload() result {
	const (
		n = 1000000
		r = 3
	)

	gen := lcg{state: 20270917} // the stream continues across rounds
	var sumV, chk, nbytes, lines int64

	for round := 0; round < r; round++ {
		name := "mb10_round_" + strconv.Itoa(round) + ".txt"

		out, err := os.Create(name)
		if err != nil {
			fatal(err)
		}
		w := bufio.NewWriterSize(out, bufSize) // section 4.12(c) pins 65536 for Go
		for i := 0; i < n; i++ {
			v := gen.nextInt()
			line := strconv.Itoa(i) + " " + strconv.FormatInt(v, 10) + "\n"
			if _, err := w.WriteString(line); err != nil {
				fatal(err)
			}
			nbytes = nbytes + int64(len(line))
		}
		if err := w.Flush(); err != nil {
			fatal(err)
		}
		if err := out.Close(); err != nil {
			fatal(err)
		}

		in, err := os.Open(name)
		if err != nil {
			fatal(err)
		}
		rd := bufio.NewReaderSize(in, bufSize) // section 4.12(c) pins 65536 for Go
		idx := 0
		for {
			line, err := rd.ReadString('\n') // section 4.12(c) pins ReadString('\n')
			if len(line) > 0 {
				line = strings.TrimSuffix(line, "\n")
				field0, field1, ok := strings.Cut(line, " ")
				if !ok {
					fatal("malformed line")
				}
				a, perr := strconv.Atoi(field0)
				if perr != nil {
					fatal(perr)
				}
				v, perr := strconv.ParseInt(field1, 10, 64)
				if perr != nil {
					fatal(perr)
				}
				if a != idx {
					fatal("index mismatch")
				}
				idx++
				lines++
				sumV = (sumV + v) % 1000000007
				chk = (chk*31 + v%1000003) % 1000003
			}
			if err != nil {
				if err == io.EOF {
					break
				}
				fatal(err)
			}
		}
		if err := in.Close(); err != nil {
			fatal(err)
		}
	}

	return result{sumV: sumV, chk: chk, nbytes: nbytes, lines: lines}
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
	fmt.Printf("MB10 %d %d %d %d\n", r.sumV, r.chk, r.nbytes, r.lines)
}
