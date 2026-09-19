// DBG-1
package main

import (
	"flag"
	"fmt"
	"os"
	"runtime/pprof"
)

const ITERATIONS int64 = 300000000
const MODULUS int64 = 1000000007

type Item struct {
	Count int64
	Name  string
}

func accumulate(items []Item, iterations int64) int64 {
	var total int64 = 0
	for i := int64(0); i < iterations; i++ {
		for _, item := range items {
			total = (total*31 + item.Count + int64(len(item.Name))) % MODULUS
		}
	}
	return total
}

var cpuprofile = flag.String("cpuprofile", "", "write cpu profile to file")

func main() {
	flag.Parse()
	if *cpuprofile != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
			panic(err)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}
	items := []Item{}
	items = append(items, Item{Count: 7, Name: "alpha"})
	items = append(items, Item{Count: 11, Name: "bravo"})
	items = append(items, Item{Count: 13, Name: "charlie"})
	checksum := accumulate(items, ITERATIONS)
	fmt.Printf("checksum=%d\n", checksum)
}
