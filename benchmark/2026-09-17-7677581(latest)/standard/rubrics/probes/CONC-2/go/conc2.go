// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
package main

import (
	"fmt"
	"sync"
)

const ITERS = 200000

var counter int64

func main() {
	var wg sync.WaitGroup
	for k := 0; k < 2; k++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < ITERS; i++ {
				counter = counter + 1
			}
		}()
	}
	wg.Wait()
	fmt.Printf("counter=%d expected=%d\n", counter, 2*ITERS)
}
