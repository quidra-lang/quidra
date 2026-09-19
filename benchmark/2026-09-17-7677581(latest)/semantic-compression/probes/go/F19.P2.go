package main

import "fmt"

// BEGIN PROBE F19.P2
import (
	"sync"
	"sync/atomic"
)

func raceFree() int64 {
	var counter int64
	var wg sync.WaitGroup
	for range 2 {
		wg.Go(func() {
			for range 1000 {
				atomic.AddInt64(&counter, 1)
			}
		})
	}
	wg.Wait()
	return counter
	// END PROBE F19.P2
}

func main() {
	fmt.Println(raceFree())
}
